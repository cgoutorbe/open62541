#Building instruction

To build the library for zephyr we use the posix compatibility of both. 

##Build Open62541
Create a build directory and navigate to it. Then use cmake to build the library using option UA_ARCHITECTURE set to zephyr_posix. If using the terminal the command line should be as follows. 
```
    mkdir build_zephyr
    cd build_zephyr
    cmake -D UA_ARCHITECTURE=zephyr_posix -D UA_ENABLE_AMALGAMATION=ON .. -G "MinGW Makefiles"
    make
```
The command will fail but the amalgamated files open62541.h and open62541.c should be generated. Import these files in your project to use the library with Zephyr. 

1. Add the .c file for compilation 
2. Add the variable UA_ARCHITECTURE_ZEPHYR_POSIX to the compilation. 
3. Add the .h file to a folder included in the compilation. 

#Setting up Zephyr
##KCONFIG
As we use the Posix capabilities of Zephyr to communicate with the library, you need to set some configuration options in the main prj.conf file. The following configuration file sets the ethernet layer on and sets up the IPv4 adress. You may use the adress you want. 

The required options are those related to ethernet and the ```CONFIG_POSIX_API=y```.

```
#ETHERNET 
CONFIG_NET_SOCKETS=y
CONFIG_NETWORKING=y
CONFIG_NET_L2_ETHERNET=y

CONFIG_NET_IPV4=y
CONFIG_NET_IPV6=n
CONFIG_NET_TCP=y


#TIME CONFIG 
CONFIG_NEWLIB_LIBC=y #posix C lang support unit of functionality

# Network address config
CONFIG_NET_CONFIG_SETTINGS=y
CONFIG_NET_CONFIG_NEED_IPV4=y
CONFIG_NET_CONFIG_MY_IPV4_ADDR="169.254.212.230"
CONFIG_NET_CONFIG_PEER_IPV4_ADDR="169.254.212.232"
CONFIG_DNS_RESOLVER=y


#MEMORY CONFIG
CONFIG_MAIN_STACK_SIZE=65536 
CONFIG_ISR_STACK_SIZE=4096
CONFIG_IDLE_STACK_SIZE=1024 
CONFIG_PRIVILEGED_STACK_SIZE=2560
CONFIG_INIT_STACKS=y
CONFIG_STACK_CANARIES=y
CONFIG_HW_STACK_PROTECTION=y

#POSIX
CONFIG_POSIX_API=y
CONFIG_POSIX_MAX_FDS=16 

```

If using the gnu arm embedded toolchain to build zephyr the following option avoids pthread conflicts.
```
CONFIG_PTHREAD_IPC=n 
#avoid pthread conflict with gnu_arm_embedded_sys

```
##CMAKE
1.Set the ```ZEPHYR_TOOLCHAIN_VARIANT``` and ```ZEPHYR_BASE``` environment variables. 
2.Add the library files for compilation. 
I placed the files in a libopen62541 folder with the .h in an /include subdirectory and the .c in /src. If using this file path you should add the files to your cmake using the following cmake commands. 
```
cmake_minimum_required(VERSION 3.13.1)
add_definitions("-DUA_ARCHITECTURE_ZEPHYR_POSIX")
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app)

target_sources(app PRIVATE src/main.c lib_open62541/src/open62541.c)

zephyr_include_directories($ENV{ZEPHYR_BASE}/include)
target_include_directories(app PRIVATE "lib_open62541/include")

zephyr_get_include_directories_for_lang_as_string(C includes)
zephyr_get_system_include_directories_for_lang_as_string(C system_includes)
zephyr_get_compile_definitions_for_lang_as_string(C definitions)
zephyr_get_compile_options_for_lang_as_string(C options)
```
 
