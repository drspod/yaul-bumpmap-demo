ifeq ($(strip $(YAUL_INSTALL_ROOT)),)
  $(error Undefined YAUL_INSTALL_ROOT (install root directory))
endif

include $(YAUL_INSTALL_ROOT)/share/pre.common.mk

BUILTIN_ASSETS+= \
	assets/WALL3_IMG.TGA;asset_wall_img_tga \
	assets/WALL3_HMAP.TGA;asset_wall_hmap_tga \
	assets/SEGA_IMG.TGA;asset_sega_img_tga \
	assets/SEGA_HMAP.TGA;asset_sega_hmap_tga

SH_PROGRAM:= test-bumpmap
SH_SRCS:= \
	test-bumpmap.c

SH_LIBRARIES:= tga
SH_CFLAGS+= -O2 -I. -save-temps=obj

IP_VERSION:= V1.000
IP_RELEASE_DATE:= 20160101
IP_AREAS:= JTUBKAEL
IP_PERIPHERALS:= JAMKST
IP_TITLE:= bump-map test
IP_MASTER_STACK_ADDR:= 0x06004000
IP_SLAVE_STACK_ADDR:= 0x06001E00
IP_1ST_READ_ADDR:= 0x06004000

M68K_PROGRAM:=
M68K_OBJECTS:=

include $(YAUL_INSTALL_ROOT)/share/post.common.mk
