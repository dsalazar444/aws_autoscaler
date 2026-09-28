# AWS Autoscaler

Autoscalador reactivo y proactivo para Auto Scaling Groups de AWS, que toma decisiones basadas en CPU y RequestCount.

## Instalación y compilación

### 1. Instalar dependencias y vcpkg

```bash
sudo pacman -S git base-devel cmake
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && ./bootstrap-vcpkg.sh
```

### 2. Configurar el proyecto con CMake (con vcpkg)

Desde la raíz del proyecto:

```bash
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=/home/daniela/Applications/vcpkg/scripts/buildsystems/vcpkg.cmake
```

**Explicación:**
- `-S .` → El código fuente está en el directorio actual
- `-B build` → Los artefactos de compilación van a la carpeta `build/` (mantiene el proyecto limpio)
- `-DCMAKE_TOOLCHAIN_FILE=...` → Indica a CMake que use vcpkg para encontrar e instalar dependencias (AWS SDK, nlohmann_json, etc.)

### 3. Compilar

```bash
cmake --build build
```

### 4. Ejecutar

```bash
./build/aws-autoscaler
```

---

## Configuración (config.json)

Archivo de configuración principal. Todos los valores están en segundos a menos que se indique lo contrario.

### AWS

- **`asgName`**: Nombre del Auto Scaling Group en AWS 
- **`tgArn`**: ARN del Target Group para obtener RequestCount

### Modo de datos

- **`falseData`**: `"True"` para usar datos simulados (random walk), `"False"` para AWS real

### Límites de instancias

- **`minInstances`**: Cantidad mínima de instancias en el ASG
- **`maxInstances`**: Cantidad máxima de instancias en el ASG

### Thresholds de CPU (porcentaje)

- **`highThresholdCpu`**: CPU por encima de esto → considerar scale-out (default: 70%)
- **`lowThresholdCpu`**: CPU por debajo de esto → considerar scale-in (default: 30%)

### Thresholds de Requests (RequestCountPerTarget promedio)

- **`highThresholdReq`**: Requests por encima de esto → considerar scale-out (default: 1000)
- **`lowThresholdReq`**: Requests por debajo de esto → considerar scale-in (default: 100)

### Ventanas de análisis (en segundos)

- **`historyWindow`**: Historial a consultar de CloudWatch (default: 1800s = 30 min)
- **`horizonWindow`**: Horizonte de predicción del analizador proactivo (default: 540s = 9 min)

### Sostenimiento de condiciones (en segundos)

Para evitar reaccionar a picos puntuales, la métrica debe mantenerse sostenida por estas ventanas:

- **`sustainedHighWindowCpu`**: CPU debe estar alta por N segundos antes de scale-out (default: 240s = 4 min)
- **`sustainedLowWindowCpu`**: CPU debe estar baja por N segundos antes de scale-in (default: 360s = 6 min)
- **`sustainedHighWindowReq`**: Requests deben estar altos por N segundos antes de scale-out (default: 180s = 3 min)

### Parámetros técnicos

- **`queryPeriod`**: Período de muestreo de CloudWatch (default: 60s = 1 min)

### Cooldown (en segundos)

Tiempo de espera después de una acción antes de permitir la siguiente:

- **`scaleOutCooldown`**: Espera tras scale-out (default: 360s = 6 min)
- **`scaleInCooldown`**: Espera tras scale-in (default: 480s = 8 min)

### Logging

- **`logPathFile`**: Ruta del archivo JSON de logs (se crea automáticamente si no existe)
- **`logGraphics`**: Ruta del archivo SVG con gráfica de métricas (se genera al final)

---

## Notas importantes

- Los archivos especificados en `logPathFile` deben existir (crear `logs/logs.json` manualmente si no existe)
- `logGraphics` se genera automáticamente al terminar la ejecución
- Para AWS real, completar `asgName` y `tgArn`, y cambiar `falseData` a `"False"`