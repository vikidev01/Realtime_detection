# Realtime Detection - Estructura de directorios

Este documento describe la ubicación y función de los archivos necesarios para el sistema **Realtime Detection**.

---

## 📂 Estructura de archivos

| Archivo / Carpeta | Ruta | Descripción |
|--------------------|------|--------------|
| **S99realtime-detection** | `/etc/init.d/` | Script de inicio del servicio Realtime Detection. Se ejecuta automáticamente al arrancar el sistema. |
| **config.ini** | `/usr/local/bin/` | Archivo de configuración principal. Contiene parámetros generales del sistema (rutas, umbrales, etc.). |
| **realtime-detection-wrapper.sh** | `/usr/local/bin/` | Script wrapper que inicia el binario principal y gestiona el entorno de ejecución. |
| **Realtime_detection_http** | `/usr/local/bin/` | Binario ejecutable principal que realiza la detección en tiempo real utilizando el modelo YOLO. |
| **yolo.cvimodel** | `/usr/local/bin/` | Modelo de detección YOLO en formato CVITEK (`.cvimodel`). Utilizado por el ejecutable principal para realizar inferencias. |
| **/etc/realtime-detection/** | Carpeta | Directorio de configuración del modo de operación. |
| **mode** | `/etc/realtime-detection/mode` | Archivo que define el modo de comunicación del sistema. Su contenido debe ser **`http`** o **`mqtt`** (una sola palabra). |

cd /etc/init.d/
sudo cp S93sscma-supervisor /home/recamera/
sudo cp S91sscma-node /home/recamera/
sudo cp S03node-red /home/recamera/
sudo cp S99user /home/recamera/
sudo rm S93sscma-supervisor
sudo rm S91sscma-node
sudo rm S03node-red
sudo rm S99user
---

## 🛠️ Ejemplo de contenido del archivo `mode`

