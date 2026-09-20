Pasos de instalación:
1. Instalar vcpkg

sudo pacman -S git base-devel cmake
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && ./bootstrap-vcpkg.sh

2. Usando el vcpkg.json, instalamos dependencias de AWS. Corra ubicado en raiz de proyecto:

cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=/home/daniela/Applications/vcpkg/scripts/buildsystems/vcpkg.cmake

3. Compile usando CMakeLists.txt

cmake --build build

4. Luego, ejecute el resultado.


Aclaraciones:

¿Qué significa 3?

Esta parte:

cmake -S . -B build

significa:

-S .
   ↑
   mi proyecto está aquí

-B build
   ↑
   coloca los archivos generados por CMake aquí

Así no contaminamos el proyecto con archivos de compilación.
Además de configurar el proyecto.

Y: -DCMAKE_TOOLCHAIN_FILE=...

le dice:

CMake, utiliza la integración de vcpkg para encontrar e instalar las dependencias. 

Luego de instalar dependencias, ahi sí usamos build, porque CMake ahora sí podrá trabajar.