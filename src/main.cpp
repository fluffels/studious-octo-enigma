#pragma warning (disable: 4267)
#pragma warning (disable: 4996)

#include <iomanip>

#include <Windows.h>

#include "Logging.h"
#include "FileSystem.cpp"
#include "Vulkan.cpp"

#include "BSPParser.cpp"
#include "BSPTextureParser.cpp"
#include "Camera.cpp"
#include "Controller.cpp"
#include "DirectInput.cpp"
#include "Mesh.cpp"
#include "Mouse.cpp"
#include "Palette.cpp"
#include "PAKParser.cpp"
#include "RenderLevel.cpp"
#include "RenderModel.cpp"
#include "RenderText.cpp"
#include "Win32.cpp"

using std::exception;
using std::setprecision;
using std::fixed;
using std::setw;

#define WIN32_CHECK(e, m) if (e != S_OK) throw new std::runtime_error(m)

#pragma pack (push, 1)
struct Uniforms {
    mat4 mvp;
    vec3 origin;
    float elapsedS;
    // NOTE(jan): GLSL pads array elements to align on 4 byte boundaries
    float light[4*12];
};
#pragma pack (pop)

const int WIDTH = 800;
const int HEIGHT = 800;

const float DELTA_MOVE_PER_S = 200.f;
const float DELTA_ROTATE_PER_S = 3.14f;
const float MOUSE_SENSITIVITY = 0.1f;
const float JOYSTICK_SENSITIVITY = 100;

bool keyboard[VK_OEM_CLEAR] = {};

VkSurfaceKHR getSurface(
    HWND window,
    HINSTANCE instance,
    const VkInstance& vkInstance
) {
    VkSurfaceKHR surface;

    VkWin32SurfaceCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    createInfo.hinstance = instance;
    createInfo.hwnd = window;

    auto result = vkCreateWin32SurfaceKHR(
        vkInstance,
        &createInfo,
        nullptr,
        &surface
    );

    if (result != VK_SUCCESS) {
        throw runtime_error("could not create win32 surface");
    } else {
        return surface;
    }
}

LRESULT
WindowProc(
    HWND    window,
    UINT    message,
    WPARAM  wParam,
    LPARAM  lParam
) {
    switch (message) {
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) PostQuitMessage(0);
            else keyboard[(uint16_t)wParam] = true;
            break;
        case WM_KEYUP:
            keyboard[(uint16_t)wParam] = false;
            break;
    }
    return DefWindowProc(window, message, wParam, lParam);
}

int
MainLoop(
    HINSTANCE instance,
    HINSTANCE prevInstance,
    LPSTR commandLine,
    int showCommand,
    PAKParser& parser
) {
    INFO("Starting...");

    LARGE_INTEGER counterFrequency;
    QueryPerformanceFrequency(&counterFrequency);
    
    WNDCLASSEX windowClassProperties = {};
    windowClassProperties.cbSize = sizeof(windowClassProperties);
    windowClassProperties.style = CS_HREDRAW | CS_VREDRAW;
    windowClassProperties.lpfnWndProc = (WNDPROC)WindowProc;
    windowClassProperties.hInstance = instance;
    windowClassProperties.lpszClassName = "MainWindowClass";
    ATOM windowClass = RegisterClassEx(&windowClassProperties);
    CHECK(windowClass, "could not create window class");

    HWND window = CreateWindowEx(
        0,
        "MainWindowClass",
        "kwark",
        WS_POPUP | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        WIDTH,
        HEIGHT,
        nullptr,
        nullptr,
        instance,
        nullptr
    );
    CHECK(window, "could not create window");

    SetWindowPos(
        window,
        HWND_TOP,
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN),
        SWP_FRAMECHANGED
    );
    ShowCursor(FALSE);

    Win32 platform(instance, window);

    Vulkan vk;
    vk.extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    createVKInstance(vk);
    vk.swap.surface = getSurface(window, instance, vk.handle);
    initVK(vk);

    BSPParser* map = parser.loadMap("start");

    auto playerStart = map->findEntityByName("info_player_start");
    auto origin = playerStart.origin;
    Camera camera;
    camera.setFOV(90);
    camera.setAR(vk.swap.extent.width, vk.swap.extent.height);
    camera.nearz = 1.f;
    camera.farz = 10000.f;
    camera.eye = { origin.x, origin.y, origin.z };
    camera.at = camera.eye;
    camera.at.x += 1;
    camera.up = { 0, 1, 0 };
    auto angle = (float)-playerStart.angle;
    camera.rotateY(angle);

    {
        FILE* save;
        auto err = fopen_s(&save, "save.dat", "r");
        if (!err) {
            fread(&camera.eye, sizeof(camera.eye), 1, save);
            fread(&camera.at, sizeof(camera.at), 1, save);
            fread(&camera.up, sizeof(camera.up), 1, save);
            fclose(save);
        }
    }

    // See progs/world.qc
    vector<string> lightstyles;
    lightstyles.push_back(string("m"));
    lightstyles.push_back("mmnmmommommnonmmonqnmmo");
    lightstyles.push_back("abcdefghijklmnopqrstuvwxyzyxwvutsrqponmlkjihgfedcba");
    lightstyles.push_back("mmmmmaaaaammmmmaaaaaabcdefgabcdefg");
    lightstyles.push_back("mamamamamama");
    lightstyles.push_back("jklmnopqrstuvwxyzyxwvutsrqponmlkj");
    lightstyles.push_back("nmonqnmomnmomomno");
    lightstyles.push_back("mmmaaaabcdefgmmmmaaaammmaamm");
    lightstyles.push_back("mmmaaammmaaammmabcdefaaaammmmabcdefmmmaaaa");
    lightstyles.push_back("aaaaaaaazzzzzzzz");
    lightstyles.push_back("mmamammmmammamamaaamammma");
    lightstyles.push_back("abcdefghijklmnopqrrqponmlkjihgfedcba");

    vector<VkCommandBuffer> levelCmds;
    renderLevel(vk, *map, levelCmds);
    initModels(vk, parser, map->entities);
    vector<VkCommandBuffer> modelCmds;
    vector<VkCommandBuffer> textCmds;

    DirectInput directInput(instance);
    Controller* controller = directInput.controller;
    Mouse* mouse = directInput.mouse;

    LARGE_INTEGER epoch = {};
    QueryPerformanceCounter(&epoch);

    LARGE_INTEGER frameStart = {}, frameEnd = {};
    int64_t frameDelta = {};
    float fps = 0;
    float frameTime = 0;

    int errorCode = 0;

    BOOL done = false;
    while (!done) {
        MSG msg;
        BOOL messageAvailable; 
        do {
            messageAvailable = PeekMessage(
                &msg,
                (HWND)nullptr,
                0, 0,
                PM_REMOVE
            );
            TranslateMessage(&msg); 
            if (msg.message == WM_QUIT) {
                done = true;
                errorCode = (int)msg.wParam;
            }
            DispatchMessage(&msg); 
        } while(!done && messageAvailable);

        if (!done) {
            char debugString[1024];
            snprintf(debugString, 1024, "%.2f FPS", fps);

            recordTextCommandBuffers(vk, textCmds, debugString);

            QueryPerformanceCounter(&frameStart);
                Uniforms uniforms = {};
                uniforms.mvp = camera.get();
                uniforms.origin = camera.eye;
                uniforms.elapsedS = (frameStart.QuadPart - epoch.QuadPart) /
                    (float)counterFrequency.QuadPart;

                int lightFrame = (int)(uniforms.elapsedS / .1f);
                for (int i = 0; i < 12; i++) {
                    auto& lightstyle = lightstyles[i];
                    int lightStyleFrame = lightFrame % lightstyle.size();
                    uniforms.light[i*4] = (lightstyle[lightStyleFrame] - 'a') / (float)('z' - 'a');
                }
                updateUniforms(vk, &uniforms, sizeof(uniforms));

                recordModelCommandBuffers(
                    vk, uniforms.elapsedS, modelCmds
                );
                vector<VkCommandBuffer> cmdss;
                auto frameBufferCount = vk.swap.framebuffers.size();
                for (unsigned i = 0; i < frameBufferCount; i++) {
                    cmdss.push_back(levelCmds[i]);
                    cmdss.push_back(modelCmds[i]);
                    cmdss.push_back(textCmds[i]);
                }
                present(vk, cmdss.data(), 3);
                resetTextCommandBuffers(vk, textCmds);
                vkFreeCommandBuffers(
                    vk.device,
                    vk.cmdPoolTransient,
                    modelCmds.size(),
                    modelCmds.data()
                );
            QueryPerformanceCounter(&frameEnd);
            // SetWindowText(window, buffer);
            frameDelta = frameEnd.QuadPart - frameStart.QuadPart;
            frameTime = (float)frameDelta / counterFrequency.QuadPart;
            fps = counterFrequency.QuadPart / (float)frameDelta;

            float deltaMove = DELTA_MOVE_PER_S * frameTime;
            if (keyboard['W']) {
                camera.forward(deltaMove);
            }
            if (keyboard['S']) {
                camera.back(deltaMove);
            }
            if (keyboard['A']) {
                camera.left(deltaMove);
            }
            if (keyboard['D']) {
                camera.right(deltaMove);
            }
            if (keyboard['F']) {
                SetWindowPos(
                    window,
                    HWND_TOP,
                    0,
                    0,
                    WIDTH,
                    HEIGHT,
                    SWP_FRAMECHANGED
                );
            }
            if (keyboard['R']) {
                camera.eye = { origin.x, origin.y, origin.z };
                camera.at = camera.eye;
                camera.at.x += 1;
                camera.up = { 0, 1, 0 };
                camera.rotateY(angle);
            }

            float deltaMouseRotate =
                MOUSE_SENSITIVITY;
            auto mouseDelta = mouse->getDelta();

            camera.rotateY((float)mouseDelta.x * deltaMouseRotate);
            camera.rotateX((float)-mouseDelta.y * deltaMouseRotate);

            float deltaJoystickRotate =
                DELTA_ROTATE_PER_S * frameTime * JOYSTICK_SENSITIVITY;
            if (controller) {
                auto state = controller->getState();

                camera.rotateY(state.rX * deltaJoystickRotate);
                camera.rotateX(-state.rY * deltaJoystickRotate);
                camera.right(state.x * deltaMove);
                camera.forward(-state.y * deltaMove);
            }
        }
    }

    {
        FILE* save;
        auto err = fopen_s(&save, "save.dat", "w");
        if (!err) {
            fwrite(&camera.eye, sizeof(camera.eye), 1, save);
            fwrite(&camera.at, sizeof(camera.at), 1, save);
            fwrite(&camera.up, sizeof(camera.up), 1, save);
            fclose(save);
        } else {
            LERROR(err);
        }
    }

    delete map;
    map = nullptr;

    return errorCode; 
}

int __stdcall
WinMain(
    HINSTANCE instance,
    HINSTANCE prevInstance,
    LPSTR commandLine,
    int showCommand
) {
    // NOTE: Initialize logging.
    initLogging();

    LPSTR pathToPAK = "PAK0.PAK";
    INFO("path to PAK file: %s", pathToPAK);
    PAKParser parser(pathToPAK);

    return MainLoop(
        instance,
        prevInstance,
        commandLine,
        showCommand,
        parser
    );
}
