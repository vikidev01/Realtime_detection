#include <ClassMapper.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sscma.h>
#include <stdio.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <numeric>
#include <regex>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>
#include "config.h"
#include "cvi_comm_video.h"
#include "cvi_sys.h"
#include "global_cfg.h"
#include "model_detector.h"
#include <iomanip>

using json = nlohmann::json;
namespace fs = std::filesystem;

std::mutex save_mutex;
std::condition_variable save_cv;
bool stop_saver = false;
std::queue<SaveTask> save_queue;
std::string folder_name       = "/mnt/sd/.cap";
std::string folder_name_train = "/mnt/sd/.cap_train";
std::string folder_name_bak   = "/usr/local/bin/.cap_bak";
bool g_first_save = true;
class ColorPalette {
public:
    static std::vector<cv::Scalar> getPalette() {
        return palette;
    }

    static cv::Scalar getColor(int index) {
        return palette[index % palette.size()];
    }

private:
    static const std::vector<cv::Scalar> palette;
};

const std::vector<cv::Scalar> ColorPalette::palette = {
    cv::Scalar(0, 255, 0),     cv::Scalar(0, 170, 255), cv::Scalar(0, 128, 255), cv::Scalar(0, 64, 255),  cv::Scalar(0, 0, 255),     cv::Scalar(170, 0, 255),   cv::Scalar(128, 0, 255),
    cv::Scalar(64, 0, 255),    cv::Scalar(0, 0, 255),   cv::Scalar(255, 0, 170), cv::Scalar(255, 0, 128), cv::Scalar(255, 0, 64),    cv::Scalar(255, 128, 0),   cv::Scalar(255, 255, 0),
    cv::Scalar(128, 255, 0),   cv::Scalar(0, 255, 128), cv::Scalar(0, 255, 255), cv::Scalar(0, 128, 128), cv::Scalar(128, 0, 255),   cv::Scalar(255, 0, 255),   cv::Scalar(128, 128, 255),
    cv::Scalar(255, 128, 128), cv::Scalar(255, 64, 64), cv::Scalar(64, 255, 64), cv::Scalar(64, 64, 255), cv::Scalar(128, 255, 255), cv::Scalar(255, 255, 128),
};

const std::vector<std::string> ClassMapper::classes= {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
        "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
        "hair drier", "toothbrush"
    };

cv::Mat preprocessImage(cv::Mat& image, ma::Model* model) {
    int ih = image.rows;
    int iw = image.cols;
    int oh = 0;
    int ow = 0;

    if (model->getInputType() == MA_INPUT_TYPE_IMAGE) {
        oh = reinterpret_cast<const ma_img_t*>(model->getInput())->height;
        ow = reinterpret_cast<const ma_img_t*>(model->getInput())->width;
    }

    cv::Mat resizedImage;
    double resize_scale = std::min((double)oh / ih, (double)ow / iw);
    int nh              = (int)(ih * resize_scale);
    int nw              = (int)(iw * resize_scale);
    cv::resize(image, resizedImage, cv::Size(nw, nh));
    int top    = (oh - nh) / 2;
    int bottom = (oh - nh) - top;
    int left   = (ow - nw) / 2;
    int right  = (ow - nw) - left;

    cv::Mat paddedImage;
    cv::copyMakeBorder(resizedImage, paddedImage, top, bottom, left, right, cv::BORDER_CONSTANT, cv::Scalar::all(0));
    cv::cvtColor(paddedImage, paddedImage, cv::COLOR_BGR2RGB);

    return paddedImage;
}

void release_camera(ma::Camera*& camera) noexcept {
    camera->stopStream();
    camera = nullptr;  // 确保线程安全
}

void release_model(ma::Model*& model, ma::engine::EngineCVI*& engine) {
    if (model)
        ma::ModelFactory::remove(model);  // 先释放model
    if (engine)
        delete engine;
    model  = nullptr;
    engine = nullptr;
}

ma::Camera* initialize_camera() noexcept {
    ma::Device* device = ma::Device::getInstance();
    ma::Camera* camera = nullptr;

    for (auto& sensor : device->getSensors()) {
        if (sensor->getType() == ma::Sensor::Type::kCamera) {
            camera = static_cast<ma::Camera*>(sensor);
            break;
        }
    }
    if (!camera) {
        MA_LOGE(TAG, "No camera sensor found");
        return camera;
    }
    if (camera->init(0) != MA_OK) {
        MA_LOGE(TAG, "Camera initialization failed");
        return camera;
    }

    ma::Camera::CtrlValue value;
    value.i32 = 0;
    if (camera->commandCtrl(ma::Camera::CtrlType::kChannel, ma::Camera::CtrlMode::kWrite, value) != MA_OK) {
        MA_LOGE(TAG, "Failed to set camera channel");
        return camera;
    }

    value.u16s[0] = 1920;
    value.u16s[1] = 1080;
    if (camera->commandCtrl(ma::Camera::CtrlType::kWindow, ma::Camera::CtrlMode::kWrite, value) != MA_OK) {
        MA_LOGE(TAG, "Failed to set camera resolution");
        return camera;
    }

    value.i32 = 5;
    camera->commandCtrl(ma::Camera::CtrlType::kFps, ma::Camera::CtrlMode::kWrite, value);

    value.i32 = 0;
    camera->commandCtrl(ma::Camera::CtrlType::kFps, ma::Camera::CtrlMode::kRead, value);

    MA_LOGI(MA_TAG, "The value of kFps is: %d", value.i32);

    value.i32 = 1;
    if (camera->commandCtrl(ma::Camera::CtrlType::kChannel, ma::Camera::CtrlMode::kWrite, value) != MA_OK) {
        MA_LOGE(TAG, "Failed to set camera channel");
        return camera;
    }
    value.u16s[0] = 1920;
    value.u16s[1] = 1080;
    if (camera->commandCtrl(ma::Camera::CtrlType::kWindow, ma::Camera::CtrlMode::kWrite, value) != MA_OK) {
        MA_LOGE(TAG, "Failed to set camera resolution");
        return camera;
    }
    value.i32 = 5;
    camera->commandCtrl(ma::Camera::CtrlType::kFps, ma::Camera::CtrlMode::kWrite, value);

    value.i32 = 0;
    camera->commandCtrl(ma::Camera::CtrlType::kFps, ma::Camera::CtrlMode::kRead, value);

    MA_LOGI(MA_TAG, "The value of kFps is: %d", value.i32);
    value.i32 = 2;
    if (camera->commandCtrl(ma::Camera::CtrlType::kChannel, ma::Camera::CtrlMode::kWrite, value) != MA_OK) {
        MA_LOGE(TAG, "Failed to set camera channel");
        return camera;
    }
    value.i32 = 5;
    camera->commandCtrl(ma::Camera::CtrlType::kFps, ma::Camera::CtrlMode::kWrite, value);

    value.i32 = 0;
    camera->commandCtrl(ma::Camera::CtrlType::kFps, ma::Camera::CtrlMode::kRead, value);

    MA_LOGI(MA_TAG, "The value of kFps is: %d", value.i32);


    camera->startStream(ma::Camera::StreamMode::kRefreshOnReturn);

    return camera;
}

size_t getCurrentRSS() {
    long rss = 0L;
    FILE* fp = nullptr;
    if ((fp = fopen("/proc/self/statm", "r")) == nullptr)
        return 0;  // 无法获取

    if (fscanf(fp, "%*s%ld", &rss) != 1) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return rss * sysconf(_SC_PAGESIZE) / 1024;  // 转换为 KB
}

ma::Model* initialize_model(const std::string& model_path) noexcept {
    ma::Model* model_null = nullptr;
    ma_err_t ret          = MA_OK;
    using namespace ma;
    auto* engine = new ma::engine::EngineCVI();
    ret          = engine->init();
    if (ret != MA_OK) {
        MA_LOGE(TAG, "engine init failed");
        delete engine;
        return model_null;
    }
    ret = engine->load(model_path);

    MA_LOGI(TAG, "engine load model %s", model_path);
    if (ret != MA_OK) {
        MA_LOGE(TAG, "engine load model failed");
        delete engine;
        return model_null;
    }

    ma::Model* model = ma::ModelFactory::create(engine);

    if (model == nullptr) {
        MA_LOGE(TAG, "model not supported");
        ma::ModelFactory::remove(model);
        delete engine;
        return model_null;
    }

    MA_LOGI(TAG, "model type: %d", model->getType());

    return model;
}

void flush_buffer(ma::Camera* camera) {
    ma_img_t tmp_frame;
    while (camera->retrieveFrame(tmp_frame, MA_PIXEL_FORMAT_JPEG) == MA_OK) {
        camera->returnFrame(tmp_frame);  // Libera inmediatamente los frames acumulados
    }
}

void cleanup(const std::string& folder_path, size_t max_images, size_t delete_batch = 30) {
    try {
        if (!fs::exists(folder_path) || !fs::is_directory(folder_path)) {
            return;
        }
        std::vector<std::string> files;
        std::regex pattern(R"(^\d{8}T\d{6}\.jpg$)");

        // Recopilar archivos válidos
        for (const auto& entry : fs::directory_iterator(folder_path)) {
            if (entry.is_regular_file() && entry.path().extension() == ".jpg") {
                std::string filename = entry.path().filename().string();
                if (std::regex_match(filename, pattern)) {
                    files.push_back(entry.path().string());
                }
            }
        }

        if (files.size() <= max_images) {
            return;
        }

        std::sort(files.begin(), files.end());
        size_t files_to_delete = std::min(files.size() - max_images, delete_batch);
        std::cout << "Limpiando " << files_to_delete << " configuración antigua" << std::endl;

        for (size_t i = 0; i < files_to_delete; ++i) {
            try {
                fs::remove(files[i]);
            } catch (const std::exception& e) {
                std::cerr << "Error borrando " << files[i] << ": " << e.what() << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error en cleanup: " << e.what() << std::endl;
    }
}

void draw_restricted_zone(cv::Mat& frame, const cv::Scalar& color) {
    std::string label = "restringido";

    auto get_label_position = [&](const cv::Point& top_left, const cv::Point& bottom_right, bool inside = true) {
        int x, y;

        if (inside) {
            // Posición dentro del rectángulo (centrado)
            x = top_left.x + (bottom_right.x - top_left.x) / 2 - 40;
            y = top_left.y + (bottom_right.y - top_left.y) / 2 + 10;
        } else {
            // Posición fuera del rectángulo (esquina superior derecha)
            x = std::min(top_left.x, bottom_right.x);
            y = std::min(top_left.y, bottom_right.y);
            x = std::max(x, 10);
            y = std::max(y, 30);
        }
        return cv::Point(x, y);
    };

    if (g_cfg.zone_type == "LINE") {
        int divider = static_cast<int>(g_cfg.z_coords[0] * (g_cfg.orientation == "VERT" ? frame.cols : frame.rows));

        if (g_cfg.orientation == "VERT") {
            cv::line(frame, {divider, 0}, {divider, frame.rows}, color, 2);

            // Posicionar label según el side
            if (g_cfg.side == "IZQ") {
                cv::putText(frame, label, {divider - 200, 30}, cv::FONT_HERSHEY_SIMPLEX, 0.7, color, 2);
                cv::arrowedLine(frame, {divider - 40, frame.rows / 2}, {divider - 10, frame.rows / 2}, color, 2);
            } else if (g_cfg.side == "DER") {
                cv::putText(frame, label, {divider + 10, 30}, cv::FONT_HERSHEY_SIMPLEX, 0.7, color, 2);
                cv::arrowedLine(frame, {divider + 40, frame.rows / 2}, {divider + 10, frame.rows / 2}, color, 2);
            }

        } else {
            cv::line(frame, {0, divider}, {frame.cols, divider}, color, 2);

            // Posicionar label según el side
            if (g_cfg.side == "ARRIBA") {
                cv::putText(frame, label, {frame.cols - 150, divider - 20}, cv::FONT_HERSHEY_SIMPLEX, 0.7, color, 2);
                cv::arrowedLine(frame, {frame.cols / 2, divider - 40}, {frame.cols / 2, divider - 10}, color, 2);
            } else if (g_cfg.side == "ABAJO") {
                cv::putText(frame, label, {frame.cols - 150, divider + 40}, cv::FONT_HERSHEY_SIMPLEX, 0.7, color, 2);
                cv::arrowedLine(frame, {frame.cols / 2, divider + 40}, {frame.cols / 2, divider + 10}, color, 2);
            }
        }

    } else if (g_cfg.zone_type == "LINE_ADV") {
        int x1 = static_cast<int>(g_cfg.z_coords[0] * frame.cols);
        int y1 = static_cast<int>(g_cfg.z_coords[1] * frame.rows);
        int x2 = static_cast<int>(g_cfg.z_coords[2] * frame.cols);
        int y2 = static_cast<int>(g_cfg.z_coords[3] * frame.rows);

        cv::line(frame, {x1, y1}, {x2, y2}, color, 2);

        // Calcular punto medio y posición del label
        cv::Point mid_point((x1 + x2) / 2, (y1 + y2) / 2);
        cv::Point label_pos;

        // Determinar posición basada en la orientación de la línea
        if (abs(x2 - x1) > abs(y2 - y1)) {  // Línea más horizontal
            label_pos = cv::Point(mid_point.x - 40, mid_point.y - 15);
        } else {  // Línea más vertical
            label_pos = cv::Point(mid_point.x + 10, mid_point.y);
        }

        cv::putText(frame, label, label_pos, cv::FONT_HERSHEY_SIMPLEX, 0.7, color, 2);

    } else if (g_cfg.zone_type == "RECT") {
        int x1 = static_cast<int>(g_cfg.z_coords[0] * frame.cols);
        int y1 = static_cast<int>(g_cfg.z_coords[1] * frame.rows);
        int x2 = static_cast<int>(g_cfg.z_coords[2] * frame.cols);
        int y2 = static_cast<int>(g_cfg.z_coords[3] * frame.rows);

        cv::rectangle(frame, {x1, y1}, {x2, y2}, color, 2);

        // Posicionar label según el side (inside/outside)
        cv::Point label_pos = get_label_position({x1, y1}, {x2, y2}, g_cfg.side == "DENTRO");
        cv::putText(frame, label, label_pos, cv::FONT_HERSHEY_SIMPLEX, 0.7, color, 2);

    } else if (g_cfg.zone_type == "POLY") {
        std::vector<cv::Point> polygon_points;
        int min_x = frame.cols, min_y = frame.rows, max_x = 0, max_y = 0;

        for (size_t i = 0; i < g_cfg.z_coords.size(); i += 2) {
            int px = static_cast<int>(g_cfg.z_coords[i] * frame.cols);
            int py = static_cast<int>(g_cfg.z_coords[i + 1] * frame.rows);
            polygon_points.push_back(cv::Point(px, py));
            min_x = std::min(min_x, px);
            min_y = std::min(min_y, py);
            max_x = std::max(max_x, px);
            max_y = std::max(max_y, py);
        }

        cv::polylines(frame, polygon_points, true, color, 2);

        // Posicionar label según el side (inside/outside)
        cv::Point label_pos = get_label_position({min_x, min_y}, {max_x, max_y}, g_cfg.side == "DENTRO");
        cv::putText(frame, label, label_pos, cv::FONT_HERSHEY_SIMPLEX, 0.7, color, 2);
    }
}

bool is_inside_restricted_zone(const cv::Point& object_center, const cv::Mat& frame) {
    if (!g_cfg.zone_enabled)
        return false;

    if (g_cfg.zone_type == "LINE") {
        int divider = static_cast<int>(g_cfg.z_coords[0] * (g_cfg.orientation == "VERT" ? frame.cols : frame.rows));

        if (g_cfg.orientation == "VERT") {
            if (g_cfg.side == "IZQ")
                return (object_center.x < divider);
            if (g_cfg.side == "DER")
                return (object_center.x > divider);

        } else if (g_cfg.orientation == "HORIZ") {
            if (g_cfg.side == "ARRIBA")
                return (object_center.y < divider);
            if (g_cfg.side == "ABAJO")
                return (object_center.y > divider);
        }

    } else if (g_cfg.zone_type == "LINE_ADV") {
        int x1 = static_cast<int>(g_cfg.z_coords[0] * frame.cols);
        int y1 = static_cast<int>(g_cfg.z_coords[1] * frame.rows);
        int x2 = static_cast<int>(g_cfg.z_coords[2] * frame.cols);
        int y2 = static_cast<int>(g_cfg.z_coords[3] * frame.rows);

        // Ecuación de línea general
        float A = y2 - y1;
        float B = x1 - x2;
        float C = x2 * y1 - x1 * y2;

        float side_val = A * object_center.x + B * object_center.y + C;

        // Decidir qué lado es válido según orientación
        if (abs(x2 - x1) > abs(y2 - y1)) {  // tendencia horizontal
            if (g_cfg.side == "ARRIBA")
                return (side_val < 0);
            if (g_cfg.side == "ABAJO")
                return (side_val > 0);
        } else {  // tendencia vertical
            if (g_cfg.side == "IZQ")
                return (side_val > 0);
            if (g_cfg.side == "DER")
                return (side_val < 0);
        }

    } else if (g_cfg.zone_type == "RECT") {
        int x1 = static_cast<int>(g_cfg.z_coords[0] * frame.cols);
        int y1 = static_cast<int>(g_cfg.z_coords[1] * frame.rows);
        int x2 = static_cast<int>(g_cfg.z_coords[2] * frame.cols);
        int y2 = static_cast<int>(g_cfg.z_coords[3] * frame.rows);

        cv::Rect zone_rect(x1, y1, x2 - x1, y2 - y1);
        bool inside = zone_rect.contains(object_center);

        if (g_cfg.side == "DENTRO") {
            if (inside) {
                cv::Point label_pos(zone_rect.x + zone_rect.width / 2, zone_rect.y + 5);
                // acá en vez de usar object_center para el label,
                // usás label_pos
            }
            return inside;
        }
        if (g_cfg.side == "FUERA")
            return !inside;

    } else if (g_cfg.zone_type == "POLY") {
        std::vector<cv::Point> polygon;
        for (size_t i = 0; i < g_cfg.z_coords.size(); i += 2) {
            int px = static_cast<int>(g_cfg.z_coords[i] * frame.cols);
            int py = static_cast<int>(g_cfg.z_coords[i + 1] * frame.rows);
            polygon.push_back(cv::Point(px, py));
        }

        bool inside = (cv::pointPolygonTest(polygon, object_center, false) >= 0);

        if (g_cfg.side == "DENTRO") {
            if (inside && !polygon.empty()) {
                cv::Point label_pos(polygon[0].x + 5, polygon[0].y + 5);
                // igual que arriba, usás label_pos para dibujar el texto
            }
            return inside;
        }
        if (g_cfg.side == "FUERA")
            return !inside;
    }

    return false;
}

void imageSaverThread() {
    while (true) {
        std::unique_lock<std::mutex> lock(save_mutex);
        save_cv.wait(lock, [] { return !save_queue.empty() || stop_saver; });

        if (stop_saver && save_queue.empty())
            break;

        SaveTask task = save_queue.front();
        save_queue.pop();
        lock.unlock();

        try {
            cv::imwrite(task.path, task.image);
            std::cout << "[SAVER] Guardada: " << task.path << std::endl;
        } catch (...) {
            std::cout << "[SAVER] Error al guardar: " << task.path << std::endl;
        }
    }
}

bool ensureFolderExists(const std::string& path) {
    try {
        if (!fs::exists(path)) fs::create_directory(path);
        if (!fs::is_directory(path)) throw std::runtime_error("No es un directorio válido");
        return true;
    } catch (...) {
        std::cout << "Error al crear/verificar carpeta: " << path << std::endl;
        return false;
    }
}


std::string model_detector(ma::Model*& model, cv::Mat& frame, bool report, bool can_alert, bool can_zone) {
    auto start = std::chrono::high_resolution_clock::now();
    if (!model)
        return R"({"error":"model is null"})";
    if (frame.empty())
        return R"({"error":"input frame is empty"})";
    
    // REMOVED: Unnecessary frame clone that was never used
    // cv::Mat frame_for_annotations = frame.clone();

    ma_img_t img;
    img.data   = (uint8_t*)frame.data;
    img.size   = static_cast<uint32_t>(frame.rows * frame.cols * frame.channels());
    img.width  = static_cast<uint32_t>(frame.cols);
    img.height = static_cast<uint32_t>(frame.rows);
    img.format = MA_PIXEL_FORMAT_RGB888;
    img.rotate = MA_PIXEL_ROTATE_0;

    auto* detector = static_cast<ma::model::Detector*>(model);
    
    // REMOVED: setConfig per frame - now done once at initialization
    // detector->setConfig(MA_MODEL_CFG_OPT_THRESHOLD, 0.5);
    
    auto start_run = std::chrono::high_resolution_clock::now();
    detector->run(&img);
    auto end_run      = std::chrono::high_resolution_clock::now();
    auto duration_run = std::chrono::duration_cast<std::chrono::milliseconds>(end_run - start_run).count();
    printf("duration_run: %ld ms\n", duration_run);
    auto results = detector->getResults();

    bool epp = false, camioneta = false, sampi = false, persona = false, save = false;
    int count = 0;

    if (can_zone && g_cfg.zone_enabled) {
        draw_restricted_zone(frame, cv::Scalar(255, 0, 0));
    }

    for (const auto& r : results) {
        std::string cls = ClassMapper::get_class(r.target);
        float x1        = (r.x - r.w / 2.0f) * frame.cols;
        float y1        = (r.y - r.h / 2.0f) * frame.rows;
        float x2        = (r.x + r.w / 2.0f) * frame.cols;
        float y2        = (r.y + r.h / 2.0f) * frame.rows;
        cv::Rect rect(cv::Point((int)x1, (int)y1), cv::Point((int)x2, (int)y2));

        if (cls == "person") {
            count++;
            if (g_cfg.toma_cap) save = true;
        }

        // Detecta SOLO camionetas en IPI
        if (cls == "car" || cls == "truck") {
            float width        = rect.width;
            float height       = rect.height;
            float aspect_ratio = height / width;

            bool trigger_alarm = false, zone_detect = false;
            std::string clase = cls;
            
            if (cls == "car") {
                trigger_alarm = true;
                clase = "camioneta"; 
            } else if (cls == "truck") {
                if (aspect_ratio < 0.65f) {
                    trigger_alarm = true;
                    clase = "camioneta"; 
                }
            }

            if (trigger_alarm) {
                if (g_cfg.centro) {
                    // Se toma el centro del objeto
                    if (can_zone && g_cfg.zone_enabled) {
                        cv::Point ob_center(static_cast<int>(r.x * frame.cols), static_cast<int>(r.y * frame.rows));
                        if (is_inside_restricted_zone(ob_center, frame)) {
                            zone_detect = true;
                            camioneta = true;
                            save = true;
                            cv::rectangle(frame, rect, cv::Scalar(0, 0, 255), 3);
                            std::stringstream ss;
                            ss << clase << " " << std::fixed << std::setprecision(2) << r.score;
                            cv::putText(frame, ss.str(), {std::max(0, (int)x1), std::max(15, (int)y1)}, cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
                        }
                    }
                } else {
                    // Se toma el borde del objeto
                    if (can_zone && g_cfg.zone_enabled) {
                        std::vector<cv::Point> rect_points = {
                            cv::Point((int)x1, (int)y1),
                            cv::Point((int)x2, (int)y1),
                            cv::Point((int)x1, (int)y2),
                            cv::Point((int)x2, (int)y2)
                        };
                        bool inside = false;
                        for (const auto& pt : rect_points) {
                            if (is_inside_restricted_zone(pt, frame)) {
                                inside = true;
                                break;
                            }
                        }
                        if (inside) {
                            camioneta = true;
                            zone_detect = true;
                            save = true;
                            cv::rectangle(frame, rect, cv::Scalar(0, 0, 255), 3);
                            std::stringstream ss;
                            ss << clase << " " << std::fixed << std::setprecision(2) << r.score;
                            cv::putText(frame, ss.str(), {std::max(0, (int)x1), std::max(15, (int)y1)}, cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
                        }
                    }
                }
                
                // Si la zona no está habilitada pero can_zone es true
                if (can_zone && !g_cfg.zone_enabled) {
                    camioneta = true;
                    save = true;
                    cv::rectangle(frame, rect, cv::Scalar(0, 0, 255), 3);
                    std::stringstream ss;
                    ss << clase << " " << std::fixed << std::setprecision(2) << r.score;
                    cv::putText(frame, ss.str(), {std::max(0, (int)x1), std::max(15, (int)y1)}, cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 0, 255), 2);
                }
            }
        }
        // Agrega aquí las condiciones para epp y sampi si las necesitas
        // if (cls == "epp") { epp = true; save = true; }
        // if (cls == "sampi") { sampi = true; save = true; }
    }

    json result_json;
    if (report)
        result_json["person_count"] = count;
    if (epp)
        result_json["epp"] = "epp";
    if (camioneta)
        result_json["camioneta"] = "camioneta";
    if (sampi)
        result_json["sampi"] = "sampi";
    if (persona)
        result_json["persona"] = "persona";

    result_json["cooldown"] = false;

    if (save) {
        
        static auto last_save_time = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto secs_since_last = std::chrono::duration_cast<std::chrono::seconds>(now - last_save_time).count();

        if (g_first_save || secs_since_last >= 60) {// Solo guardar si pasó el cooldown
            last_save_time = now;

            std::string target_folder = folder_name;
            bool use_backup           = false;

            if (!ensureFolderExists(folder_name)) {
                use_backup = true;
                target_folder = folder_name_bak;
            }
            // Nos aseguramos que la carpeta final exista
            bool can_save_image = ensureFolderExists(target_folder);
            if (can_save_image) {
                try {
                    auto now       = std::chrono::system_clock::now();
                    auto in_time_t = std::chrono::system_clock::to_time_t(now);
                    std::stringstream datetime_ss;
                    datetime_ss << std::put_time(std::localtime(&in_time_t), "%Y%m%dT%H%M%S");

                    std::string filename     = target_folder + "/" + datetime_ss.str() + ".jpg";
                    std::string filename_raw = folder_name_train + "/" + datetime_ss.str() + "_train.jpg";

                    cv::Mat image_bgr, image_bgr_raw;
                    if (frame.channels() == 3) {
                        cv::cvtColor(frame, image_bgr, cv::COLOR_RGB2BGR);
                        cv::cvtColor(frame, image_bgr_raw, cv::COLOR_RGB2BGR);  // Use frame directly
                    } else {
                        image_bgr     = frame.clone();
                        image_bgr_raw = frame.clone();  // Use frame directly
                    }
                    {
                        std::lock_guard<std::mutex> lock(save_mutex);
                        save_queue.push({image_bgr.clone(), filename, false});
                        if (g_cfg.train)
                            save_queue.push({image_bgr_raw.clone(), filename_raw, true});
                    }
                    save_cv.notify_all();
                    std::cout << "[QUEUE] Imagen encolada para guardar\n";

                } catch (...) {
                    std::cout << "Error al encolar imagen\n";
                }
            } 
        }else {
            std::cout << "[QUEUE] Ignorada por cooldown\n";
        }
        g_first_save = false; 
    }

    auto end      = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    printf("Duración: %ld ms\n", duration);
    return result_json.dump();
}