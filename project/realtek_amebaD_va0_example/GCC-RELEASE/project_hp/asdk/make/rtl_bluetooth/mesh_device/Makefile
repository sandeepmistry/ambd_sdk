include $(MAKE_INCLUDE_GEN)

.PHONY: all clean

include ../amebad_bsp/Makefile

SDK_DIR = $(BASEDIR)/component/common/bluetooth/realtek/sdk
MESH_DIR = $(SDK_DIR)/example/mesh

#*****************************************************************************#
#                        INCLUDE FILES                                        #
#*****************************************************************************#
MODULE_IFLAGS += \
   -I$(MESH_DIR)/lib/cmd \
   -I$(MESH_DIR)/lib/common \
   -I$(MESH_DIR)/lib/gap \
   -I$(MESH_DIR)/lib/inc \
   -I$(MESH_DIR)/lib/inc/amebad \
   -I$(MESH_DIR)/lib/model \
   -I$(MESH_DIR)/lib/model/realtek \
   -I$(MESH_DIR)/lib/platform \
   -I$(MESH_DIR)/lib/profile \
   -I$(MESH_DIR)/lib/utility \
   -I$(MESH_DIR)/device \
   -I$(SDK_DIR)/board/amebad/src/vendor_cmd
 
MODULE_IFLAGS += -DCONFIG_FTL_ENABLED -DMESH_DEVICE -DCONFIG_PLATFORM_8721D
MODULE_IFLAGS += -include app_flags.h

#*****************************************************************************#
#                        DEVICE APP FILES                                     #
#*****************************************************************************#
CSRC += \
    $(MESH_DIR)/lib/cmd/data_uart.c \
    $(MESH_DIR)/lib/profile/datatrans_server.c \
    $(MESH_DIR)/lib/profile/datatrans_service.c \
    $(MESH_DIR)/lib/model/realtek/datatrans_model_server.c \
    $(MESH_DIR)/lib/model/realtek/ping_control.c \
    $(MESH_DIR)/lib/model/realtek/tp_control.c \
    $(MESH_DIR)/lib/model/health_server.c \
    $(MESH_DIR)/device/app_task.c \
    $(MESH_DIR)/device/device_app.c \
    $(MESH_DIR)/lib/common/ping_app.c \
    $(MESH_DIR)/lib/common/datatrans_server_app.c \
    $(MESH_DIR)/lib/cmd/mesh_cmd.c \
    $(MESH_DIR)/lib/cmd/test_cmd.c \
    $(MESH_DIR)/device/device_cmd.c \
    $(MESH_DIR)/device/ble_app_main.c \
    $(SDK_DIR)/board/amebad/src/vendor_cmd/vendor_cmd.c

OBJS = $(CSRC:.c=.o)
TOBJS = $(notdir $(OBJS))
LOBJS = $(addprefix objs/,$(TOBJS))

#*****************************************************************************#
#                            LINK LIBRARY                                     #
#*****************************************************************************#
#LINK_APP_LIB += $(MESH_DIR)/lib/lib/btmesh_dev.a

#*****************************************************************************#
#                        RULES TO GENERATE TARGETS                            #
#*****************************************************************************#
# LIBBLUETOOTH_OBJS_PATH = $(shell pwd)/objs
# Define the Rules to build the core targets
all: CORE_TARGETS COPY_RAM_OBJS
	# mkdir -p $(LIBBLUETOOTH_OBJS_PATH)
	# for i in $(OBJS); do \
	# 	 $(COPY)  $$i $(LIBBLUETOOTH_OBJS_PATH) -f;\
	# 	done
	# $(REMOVE) *.i
	# $(REMOVE) *.s
	# $(REMOVE) *.d

#*****************************************************************************#
#              GENERATE OBJECT FILE
#*****************************************************************************#
CORE_TARGETS: $(OBJS)

#*****************************************************************************#
#                        RULES TO CLEAN TARGETS                               #
#*****************************************************************************#
clean: CLEAN_OBJS
	$(REMOVE) *.o
	$(REMOVE) *.i
	$(REMOVE) *.s
	$(REMOVE) *.d

-include $(DEPS)
