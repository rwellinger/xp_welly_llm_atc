#include "atc_ui.hpp"

#include <XPLMDisplay.h>

#include <imgui.h>
#include <imgui_impl_opengl2.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace atc_ui {

static bool visible = false;
static XPLMWindowID window_id = nullptr;

static void draw_window(XPLMWindowID id, void*) {
    if (!visible) return;

    int left, top, right, bottom;
    XPLMGetWindowGeometry(id, &left, &top, &right, &bottom);

    ImGui_ImplOpenGL2_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(left), static_cast<float>(top)),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(static_cast<float>(right - left), static_cast<float>(top - bottom)),
        ImGuiCond_Always);

    ImGui::Begin("Welly's ATC", &visible,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);
    ImGui::Text("ATC not yet initialized");
    ImGui::End();

    ImGui::Render();
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}

static int handle_mouse(XPLMWindowID, int, int, XPLMMouseStatus, void*) { return 1; }
static XPLMCursorStatus handle_cursor(XPLMWindowID, int, int, void*) {
    return xplm_CursorDefault;
}
static int handle_wheel(XPLMWindowID, int, int, int, int, void*) { return 1; }
static void handle_key(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {}

void init() {
    ImGui::CreateContext();
    ImGui_ImplOpenGL2_Init();

    XPLMCreateWindow_t params{};
    params.structSize = sizeof(params);
    params.left = 100;
    params.top = 500;
    params.right = 500;
    params.bottom = 200;
    params.visible = 0;
    params.drawWindowFunc = draw_window;
    params.handleMouseClickFunc = handle_mouse;
    params.handleCursorFunc = handle_cursor;
    params.handleMouseWheelFunc = handle_wheel;
    params.handleKeyFunc = handle_key;
    params.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
    params.layer = xplm_WindowLayerFloatingWindows;

    window_id = XPLMCreateWindowEx(&params);
    XPLMSetWindowTitle(window_id, "Welly's ATC");
}

void stop() {
    if (window_id) {
        XPLMDestroyWindow(window_id);
        window_id = nullptr;
    }
    ImGui_ImplOpenGL2_Shutdown();
    ImGui::DestroyContext();
}

void toggle() {
    visible = !visible;
    if (window_id) {
        XPLMSetWindowIsVisible(window_id, visible ? 1 : 0);
    }
}

void draw() {
    if (visible && window_id) {
        draw_window(window_id, nullptr);
    }
}

}  // namespace atc_ui
