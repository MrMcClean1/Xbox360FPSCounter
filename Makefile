# PC-side build: unit tests and overlay preview. No XDK needed.
# The Xbox 360 DLL is built separately with build.bat (XDK).

CC      ?= gcc
CFLAGS  ?= -std=c89 -pedantic -Wall -Wextra -Wno-long-long -O2 -Icore
OUT     ?= out

CORE = core/fps_core.c core/fps_core.h

.PHONY: all test preview clean

all: test preview

$(OUT):
	mkdir -p $(OUT)

$(OUT)/test_core: tests/test_core.c $(CORE) | $(OUT)
	$(CC) $(CFLAGS) tests/test_core.c core/fps_core.c -o $@

$(OUT)/preview: tools/preview.c $(CORE) | $(OUT)
	$(CC) $(CFLAGS) tools/preview.c core/fps_core.c -o $@

test: $(OUT)/test_core
	./$(OUT)/test_core

preview: $(OUT)/preview
	./$(OUT)/preview $(OUT)/preview_bright.ppm bright 58 fps.ini
	./$(OUT)/preview $(OUT)/preview_dark.ppm dark 58 fps.ini
	./$(OUT)/preview $(OUT)/preview_busy.ppm busy 58 fps.ini

clean:
	rm -rf $(OUT)
