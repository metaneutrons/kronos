.PHONY: all disk qemu run docs clean

QEMU_COMMIT := v10.2.2
QEMU_DIR := vendor/qemu
QEMU_BIN := $(QEMU_DIR)/build/qemu-system-i386

all: disk qemu

# --- Download & build disk image ---
disk: downloads
	./scripts/build-disk.sh

downloads:
	@mkdir -p downloads

# --- Build QEMU with custom devices ---
qemu: $(QEMU_BIN)

$(QEMU_BIN): $(QEMU_DIR)/hw/usb/kronos-nks4.c $(QEMU_DIR)/hw/isa/kronos-keybed.c
	cd $(QEMU_DIR) && mkdir -p build && cd build && \
		../configure --target-list=i386-softmmu --disable-docs --disable-gtk --disable-sdl && \
		ninja -C . qemu-system-i386

$(QEMU_DIR)/hw/usb/kronos-nks4.c: src/qemu/kronos-nks4.c | $(QEMU_DIR)
	cp $< $@
	@grep -q 'kronos-nks4' $(QEMU_DIR)/hw/usb/meson.build || \
		echo "system_ss.add(when: 'CONFIG_USB_NKS4', if_true: files('kronos-nks4.c'))" >> $(QEMU_DIR)/hw/usb/meson.build
	@grep -q 'USB_NKS4' $(QEMU_DIR)/hw/usb/Kconfig || \
		printf '\nconfig USB_NKS4\n    bool\n    default y\n    depends on USB\n' >> $(QEMU_DIR)/hw/usb/Kconfig

$(QEMU_DIR)/hw/isa/kronos-keybed.c: src/qemu/kronos-keybed.c | $(QEMU_DIR)
	cp $< $@
	@grep -q 'kronos-keybed' $(QEMU_DIR)/hw/isa/meson.build || \
		echo "system_ss.add(when: 'CONFIG_KRONOS_KEYBED', if_true: files('kronos-keybed.c'))" >> $(QEMU_DIR)/hw/isa/meson.build
	@grep -q 'KRONOS_KEYBED' $(QEMU_DIR)/hw/isa/Kconfig || \
		printf '\nconfig KRONOS_KEYBED\n    bool\n    default y\n    depends on ISA_BUS\n' >> $(QEMU_DIR)/hw/isa/Kconfig

$(QEMU_DIR):
	git submodule update --init --depth 1 vendor/qemu

# --- Run QEMU ---
run: $(QEMU_BIN) local/kronos.img
	./scripts/run.sh

local/kronos.img: disk
	@true

# --- Documentation ---
docs: docs-html

docs-html: .site_src/docs
	mkdocs build

docs-serve: .site_src/docs
	mkdocs serve

docs-pdf:
	cd docs && pandoc \
		00-frontmatter.md 01-architecture.md 02-emulation.md \
		03-usb-device.md 04-panel-protocol.md 05-video.md \
		06-events.md 07-audio.md 08-fifo.md 09-modules.md \
		10-security.md 11-esp32.md 12-appendix.md \
		--filter pandoc-crossref \
		--pdf-engine=xelatex \
		-o ../local/kronos-reference.pdf
	@echo "PDF: local/kronos-reference.pdf"

.site_src/docs: docs/*.md docs/img/* scripts/preprocess_docs.py
	python3 scripts/preprocess_docs.py docs/ .site_src/docs/
	cp docs/index.md .site_src/docs/index.md 2>/dev/null || true

# --- Clean ---
clean:
	rm -rf $(QEMU_DIR)/build local/kronos.img local/bzImage-korg downloads/
