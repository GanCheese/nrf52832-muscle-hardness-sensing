cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(muscle_stiffness)

if(CONFIG_APP_DUAL_I2C_Z_FIFO)
target_sources(app PRIVATE src/dual_i2c_z_fifo.c)
elseif(CONFIG_APP_DUAL_I2C_Z)
target_sources(app PRIVATE src/dual_i2c_z.c)
elseif(CONFIG_APP_IMU_I2C_DIAG)
target_sources(app PRIVATE src/i2c_diag.c)
else()
target_sources(app PRIVATE
  src/main.c
  src/icm20948.c
  src/max98357a.c
)
endif()

target_include_directories(app PRIVATE include)
