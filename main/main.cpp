#include <iostream>
#include <signal.h>
#include <syslog.h>
#include <thread> 
#include <atomic> 
#include <unistd.h>
#include <mutex>
#include "version.h"
#include "daemon.h"
#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include "rtsp_demo.h"
#include "video.h"
#include "cvi_sys.h"
#include "cvi_comm_video.h"
#include "model_detector.h"  
#include "connect.h"    
#include "hv/hlog.h"
#include "global_cfg.h"
#include "config.h"
#include "hv/EventLoop.h"
using json = nlohmann::json;

static ma::Model* g_model = nullptr;
static std::mutex g_det_mutex;
connectivity_mode_t connectivity_mode = CONNECTIVITY_MODE_MQTT;  

std::chrono::steady_clock::time_point g_last_object_alert = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point g_last_zone_alert   = std::chrono::steady_clock::now();
std::chrono::steady_clock::time_point g_last_report= std::chrono::steady_clock::now();

std::atomic<bool> time_sync_running{true};

void stopPeriodicTimeSync() {
    time_sync_running = false;
}

static CVI_VOID app_ipcam_ExitSig_handle(CVI_S32 signo) {
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    {
        std::lock_guard<std::mutex> lock(save_mutex);
        stop_saver = true;
    }
    save_cv.notify_all();
    if ((SIGINT == signo) || (SIGTERM == signo)) {
        stopPeriodicTimeSync();
        deinitVideo();
        deinitRtsp();

    }
    exit(-1);
}

void startPeriodicTimeSync() {
    std::thread([](){
        // Delay inicial de 30 segundos
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        // Luego ejecutar periódicamente
        while (time_sync_running) {
            if (connectivity_mode == CONNECTIVITY_MODE_HTTP) {
                printf("[TIME SYNC] Solicitando sincronización de hora...\n");
                solicitarHora();
            }
            
            // Esperar el intervalo configurado (usa un valor por defecto si no existe)
            int sync_interval = g_cfg.time_sync_interval > 0 ? g_cfg.time_sync_interval : 3600;
            std::this_thread::sleep_for(std::chrono::seconds(sync_interval));
        }
    }).detach();
}

static int fpRunYolo_CH0(void* pData, void* pArgs, void* pUserData) {
    if (!g_model) return CVI_FAILURE;

    VIDEO_FRAME_INFO_S* vinfo = (VIDEO_FRAME_INFO_S*)pData;
    if (!vinfo) return CVI_FAILURE;
    VIDEO_FRAME_S* f = &vinfo->stVFrame;

    if (f->u32Length[0] == 0) return CVI_FAILURE;

    f->pu8VirAddr[0] = (CVI_U8*)CVI_SYS_Mmap(f->u64PhyAddr[0], f->u32Length[0]);
    if (!f->pu8VirAddr[0]) {
        CVI_TRACE_LOG(CVI_DBG_ERR, "CVI_SYS_Mmap failed\n");
        return CVI_FAILURE;
    }

    cv::Mat frame_rgb888(f->u32Height, f->u32Width, CV_8UC3, f->pu8VirAddr[0]);

    auto now = std::chrono::steady_clock::now();
    bool report         = (now - g_last_report)  >= std::chrono::seconds(g_cfg.report);
    bool can_alert      = (now - g_last_object_alert)   >= std::chrono::seconds(g_cfg.cooldown);
    bool can_zone       = (now - g_last_zone_alert)     >= std::chrono::seconds(g_cfg.cooldown);
    
    std::string result_json_str;
    {
        std::lock_guard<std::mutex> lk(g_det_mutex);
        result_json_str = model_detector(g_model, frame_rgb888, report, can_alert, can_zone);
    }
    try {
        json parsed = json::parse(result_json_str);
        process_detection_results(parsed, f->pu8VirAddr[0], g_last_object_alert, g_last_zone_alert, g_last_report);
    } catch (...) {}

    CVI_SYS_Munmap(f->pu8VirAddr[0], f->u32Length[0]);
    return CVI_SUCCESS;
}

connectivity_mode_t parse_mode(int argc, char* argv[]) {
    std::string mode = "mqtt";  // valor por defecto

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--mode") == 0) && (i + 1 < argc)) {
            mode = argv[i + 1];
        }
    }
    if (mode == "http") return CONNECTIVITY_MODE_HTTP;
    if (mode == "mqtt") return CONNECTIVITY_MODE_MQTT;

    std::cerr << "Error: modo inválido '" << mode << "'. Use --mode mqtt|http\n";
    exit(1);
}

int main(int argc, char* argv[]) {
    printf("[DEBUG] Starting VanDetect...\n");
    load_config(PATH_CONF);
    printf("[DEBUG] Config loaded\n");
    
    connectivity_mode = parse_mode(argc, argv);
    printf("[DEBUG] Mode parsed: %d\n", connectivity_mode);
    
    signal(SIGINT, app_ipcam_ExitSig_handle);
    signal(SIGTERM, app_ipcam_ExitSig_handle);
    printf("[DEBUG] Signals configured\n");
    
    initWiFi();
    initHttpd();
    initConnectivity(connectivity_mode);

    printf("[DEBUG] About to call initVideo()\n");
    if (initVideo()) {
        printf("[ERROR] initVideo() falló\n");
        return -1;
    }
    printf("[DEBUG] initVideo() succeeded\n");

    video_ch_param_t param;
    
    param.format = VIDEO_FORMAT_DEFAULT;
    param.width  = VIDEO_WIDTH_DEFAULT;
    param.height = VIDEO_HEIGHT_DEFAULT;
    param.fps    = VIDEO_FPS_DEFAULT;

    printf("[DEBUG] About to call setupVideo()\n");
    setupVideo(VIDEO_CH0, &param);
    printf("[DEBUG] setupVideo() succeeded\n");
    
    printf("[DEBUG] About to register video frame handler\n");
    int ret = registerVideoFrameHandler(VIDEO_CH0, 0, fpRunYolo_CH0, NULL);
    printf("[DEBUG] Video frame handler registered\n");
    
    // Usar la constante definida para la ruta del modelo
    printf("[DEBUG] About to initialize model\n");
    g_model = initialize_model("/usr/local/bin/yolo11n_cv181x_int8.cvimodel");
    if (!g_model) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Model initialization failed");

        deinitRtsp();
        deinitVideo();
        return -1;
    }
    printf("[DEBUG] Model initialized successfully\n");
    
    // Configure model ONCE during initialization instead of per-frame
    auto* detector = static_cast<ma::model::Detector*>(g_model);
    detector->setConfig(MA_MODEL_CFG_OPT_THRESHOLD, 0.5);
    printf("[OPTIMIZATION] Model configured once at startup\n");
    std::thread saver(imageSaverThread);
    saver.detach();
    startVideo(false, false);  // mirror=false, flip=false
    
    startPeriodicTimeSync();
    sendMacId();
    
    printf("[INFO] VanDetect started - model and detection running\n");
    while (1) sleep(1);

    return 0;
}