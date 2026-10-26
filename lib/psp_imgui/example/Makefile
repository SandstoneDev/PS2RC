# psp_imgui example (pspsdk): make  ->  EBOOT.PBP
TARGET   = pimgui_example
vpath %.cpp .. ../third_party/imgui
UI_OBJS  = psp_imgui.o imgui.o imgui_draw.o imgui_widgets.o imgui_tables.o
OBJS     = main.o ui.o
INCDIR   = .. ../third_party/imgui
CFLAGS   = -Os -G0 -Wall -ffunction-sections -fdata-sections
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -w '-DIMGUI_USER_CONFIG="pimgui_imconfig.h"'
LIBS     = -lpspgu -lm
EXTRA_TARGETS    = EBOOT.PBP
PSP_EBOOT_TITLE  = psp_imgui example

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

# ImGui + psp_imgui partial-linked with --gc-sections from the C API this program uses: the unused ImGui code goes.
# (Not --gc-sections on the final link: it would also drop the PSP import stub sections.)
ui.o: $(UI_OBJS)
	psp-ld -r --gc-sections -u pimgui_create -u pimgui_new_frame -u pimgui_render -u pimgui_destroy 	    -u pimgui_window_begin -u pimgui_window_end -u pimgui_text_colored -u pimgui_separator -u pimgui_rows_begin 	    -u pimgui_rows_end -u pimgui_row -u pimgui_row_int -u pimgui_row_choice -u pimgui_row_bool 	    -u pimgui_row_button -u pimgui_marquee -u pimgui_rows_focus_time -u pimgui_hint -o $@ $^
