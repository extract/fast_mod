obj-m += fast_mod.o

# Headers for the kernel module
ccflags-y := -I$(PWD)

GEN_HEADER := shadow_structs.h

.PHONY: all clean test

all: $(GEN_HEADER) trigger
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

# Compiling the userspace app
trigger: trigger.c fast_mod_uapi.h $(GEN_HEADER)
	$(CC) $(CFLAGS) trigger.c -lpthread -o trigger

# Rule to generate the shadow header
$(GEN_HEADER): pahole_extractor.sh
	chmod +x pahole_extractor.sh
	./pahole_extractor.sh > $(GEN_HEADER)

clean:
	@# The '@' hides the command itself, '-' ignores errors if mod isn't loaded
	-sudo rmmod fast_mod 2>/dev/null || true
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f trigger $(GEN_HEADER)

test: all
	@echo "Checking for existing module..."
	@if lsmod | grep -q fast_mod; then \
		echo "Found existing fast_mod. Removing..."; \
		sudo rmmod fast_mod; \
	fi
	@sudo dmesg -C
	@sudo insmod fast_mod.ko
	@printf "\n=========== RUN LOGS =============\n"
	@sudo ./trigger
	@printf "\n\n========== KERNEL LOGS============\n"
	@sudo rmmod fast_mod
	@sudo dmesg
