ifeq ($(SYSDRV_PARAM), )
	SYSDRV_PARAM:=../../Makefile.param
	include $(SYSDRV_PARAM)
endif

CURRENT_DIR := $(shell pwd)
INC_FLAGS_MOTOR := -I$(CURRENT_DIR)/src/
SRC := $(wildcard $(CURRENT_DIR)/src/*.c)
OBJ := $(SRC:%.c=%.o)
M_OUT_DIR ?= ../out

export OBJ
export INC_FLAGS_MOTOR

MODULE_NAME := bhe-enclave

build_target := modules


all: $(build_target)
	@echo "build $(MODULE_NAME) done"

.PHONY: modules clean

$(MODULE_NAME)-objs := bhe-enclave.o
obj-m := $(MODULE_NAME).o

modules:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_DIR) M=$(shell pwd) $@ -j12
	cp $(shell pwd)/bhe-enclave.ko $(M_OUT_DIR)
	@rm -rf *.o *~ .depend .*.cmd  *.mod.c .tmp_versions *.symvers modules.order *.mod
	

clean:
	@rm -rf *.o *~ .depend .*.cmd  *.mod.c .tmp_versions *.ko *.symvers modules.order *.mod
