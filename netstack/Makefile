#---------------------------------------------------------------------------------
# libgcnet: our copy of libogc2's network stack (lwIP + BBA/ETH2GC drivers),
# replacing libbba. Taken from libogc2 commit f8f62e1; built with the same
# flags libogc2 uses for libbba.
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPRO)/libogc2/gamecube_rules

LWIP	:=	lwip
BUILD	:=	build
LIB		:=	lib/libgcnet.a

SRCS	:=	$(LWIP)/network.c $(LWIP)/netio.c $(LWIP)/arch/gc/netif/gcif.c \
			$(addprefix $(LWIP)/core/, inet.c mem.c dhcp.c raw.c memp.c netif.c pbuf.c \
				stats.c sys.c tcp.c tcp_in.c tcp_out.c udp.c) \
			$(addprefix $(LWIP)/core/ipv4/, icmp.c ip.c ip_frag.c ip_addr.c) \
			$(addprefix $(LWIP)/netif/, etharp.c loopif.c enc28j60if.c w5500if.c w6x00if.c)

OBJS	:=	$(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
vpath %.c $(sort $(dir $(SRCS)))

# "." provides libogc/*.inl, internal headers that libogc2 does not install
INCLUDES	:=	-I. -I$(LWIP)/include -I$(LWIP)/include/ipv4 -I$(LWIP)/include/netif \
				-I$(LIBOGC_INC) -I$(LIBOGC_INC)/ogc -I$(LIBOGC_INC)/ogc/machine

CFLAGS	:=	-DLIBOGC_INTERNAL -g -O2 -fno-strict-aliasing -Wall \
			-Wno-address-of-packed-member -Wno-prio-ctor-dtor \
			-DGEKKO -mcpu=750 -meabi -msdata=sysv -mhard-float \
			-ffunction-sections -fdata-sections -DHW_DOL -Wa,-mgekko \
			$(INCLUDES)

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	@rm -f $@
	@echo archiving ... $(notdir $@)
	@$(AR) -rcs $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	@echo $(notdir $<)
	@$(CC) -MMD -MP $(CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD) lib

-include $(OBJS:.o=.d)
