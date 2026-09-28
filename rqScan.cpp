#include <windows.h>
#include <mmsystem.h>
#include <opencv2/opencv.hpp>
#include <ZXing/ReadBarcode.h>
#include <string>
#include <algorithm>

// Global Handles and Synchronization States
HWND g_hMasterWindow = NULL;
HWND g_hCaptureBtn = NULL;
bool g_bCameraAvailable = false;
bool g_bCameraRunning = false;

// Selection Overlay Coordinate Tracking Struct
struct SelectionRect {
    POINT start;
    POINT end;
    bool drawing = false;
    bool done = false;
} g_selection;

// Function Declarations
LRESULT CALLBACK WndProc ( HWND, UINT, WPARAM, LPARAM );
LRESULT CALLBACK OverlayProc ( HWND, UINT, WPARAM, LPARAM );
void CheckCameraAvailability ();
void HandleCaptureAction ();
bool ProcessImageBuffer ( const cv::Mat &frame, std::string &outText, bool isScreenCapture );
void ExecuteScreenCaptureWorkflow ();
DWORD WINAPI CameraThreadProc ( LPVOID lpParam );

// Universal helper to pass a pixel matrix directly to ZXing using standard strings
bool ProcessImageBuffer ( const cv::Mat &frame, std::string &outText, bool isScreenCapture ) {
    if ( frame.empty () ) return false;

    // Explicitly handle color depths safely based on source pipeline
    ZXing::ImageFormat format = isScreenCapture ? ZXing::ImageFormat::BGRA : ZXing::ImageFormat::BGR;
    ZXing::ImageView image ( frame.data, frame.cols, frame.rows, format );

    auto result = ZXing::ReadBarcode ( image );
    if ( result.isValid () ) {
        outText = result.text (); // Pure 8-bit ANSI string extraction
        return true;
    }
    return false;
}

// Check hardware environment using DirectShow for safe Wine handshakes
void CheckCameraAvailability () {
    cv::VideoCapture cap ( 0, cv::CAP_DSHOW );
    g_bCameraAvailable = cap.isOpened ();
    if ( g_bCameraAvailable ) {
        cap.release ();
    }
}

int WINAPI WinMain ( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow ) {
    CheckCameraAvailability ();

    const char CLASS_NAME[] = "QRMasterWindowClass";
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)( COLOR_WINDOW + 1 );
    wc.hCursor = LoadCursorA ( NULL, IDC_ARROW );
    RegisterClassA ( &wc );

    // Register our screen capture selection transparency class layer
    WNDCLASSA overlayWc = {};
    overlayWc.lpfnWndProc = OverlayProc;
    overlayWc.hInstance = hInstance;
    overlayWc.lpszClassName = "QRScreencapOverlayClass";
    overlayWc.hbrBackground = (HBRUSH)GetStockObject ( BLACK_BRUSH );
    overlayWc.hCursor = LoadCursorA ( NULL, IDC_CROSS );
    RegisterClassA ( &overlayWc );

    g_hMasterWindow = CreateWindowExA (
        0, CLASS_NAME, "QR Code Master Console",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 200,
        NULL, NULL, hInstance, NULL
    );

    if ( g_hMasterWindow == NULL ) return 0;

    g_hCaptureBtn = CreateWindowA (
        "BUTTON", "Capture QR",
        WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
        120, 60, 150, 40,
        g_hMasterWindow, (HMENU)1, hInstance, NULL
    );

    ShowWindow ( g_hMasterWindow, nCmdShow );
    UpdateWindow ( g_hMasterWindow );

    MSG msg = {};
    while ( GetMessageA ( &msg, NULL, 0, 0 ) ) {
        TranslateMessage ( &msg );
        DispatchMessageA ( &msg );
    }
    return 0;
}

LRESULT CALLBACK WndProc ( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam ) {
    switch ( uMsg ) {
        case WM_COMMAND:
            if ( LOWORD ( wParam ) == 1 ) {
                HandleCaptureAction ();
            }
            break;
        case WM_DESTROY:
            PostQuitMessage ( 0 );
            return 0;
    }
    return DefWindowProcA ( hwnd, uMsg, wParam, lParam );
}

void HandleCaptureAction () {
    if ( g_bCameraAvailable ) {
        int response = MessageBoxA ( g_hMasterWindow,
            "A camera was detected!\n\nWould you like to use the Live Camera?\n(Select 'No' to use Target Screen Capture instead)",
            "Select Source Target", MB_YESNOCANCEL | MB_ICONQUESTION );

        if ( response == IDYES ) {
            CreateThread ( NULL, 0, CameraThreadProc, NULL, 0, NULL );
        } else if ( response == IDNO ) {
            ExecuteScreenCaptureWorkflow ();
        }
    } else {
        ExecuteScreenCaptureWorkflow ();
    }
}

// --- VISUAL INTERACTIVE SCREEN CAPTURE OVERLAY LAYER ---
LRESULT CALLBACK OverlayProc ( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam ) {
    switch ( uMsg ) {
        case WM_LBUTTONDOWN:
            g_selection.start.x = LOWORD ( lParam );
            g_selection.start.y = HIWORD ( lParam );
            g_selection.end = g_selection.start;
            g_selection.drawing = true;
            return 0;
        case WM_MOUSEMOVE:
            if ( g_selection.drawing ) {
                HDC hdc = GetDC ( hwnd );
                HPEN hPen = CreatePen ( PS_DOT, 1, RGB ( 255, 0, 0 ) );
                SelectObject ( hdc, hPen );
                SetROP2 ( hdc, R2_NOTXORPEN );

                // Erase previous bounding box dimensions 
                Rectangle ( hdc, g_selection.start.x, g_selection.start.y, g_selection.end.x, g_selection.end.y );
                g_selection.end.x = LOWORD ( lParam );
                g_selection.end.y = HIWORD ( lParam );
                // Draw new bounding box updates
                Rectangle ( hdc, g_selection.start.x, g_selection.start.y, g_selection.end.x, g_selection.end.y );

                DeleteObject ( hPen );
                ReleaseDC ( hwnd, hdc );
            }
            return 0;
        case WM_LBUTTONUP:
            if ( g_selection.drawing ) {
                g_selection.drawing = false;
                g_selection.done = true;
                PostMessageA ( hwnd, WM_CLOSE, 0, 0 );
            }
            return 0;
        case WM_KEYDOWN:
            if ( wParam == VK_ESCAPE ) {
                g_selection.done = false;
                DestroyWindow ( hwnd );
            }
            return 0;
    }
    return DefWindowProcA ( hwnd, uMsg, wParam, lParam );
}

cv::Mat CaptureTargetRegion ( int x, int y, int w, int h ) {
    HDC hScreenDC = GetDC ( NULL );
    HDC hMemoryDC = CreateCompatibleDC ( hScreenDC );
    HBITMAP hBitmap = CreateCompatibleBitmap ( hScreenDC, w, h );
    HGDIOBJ hOldBitmap = SelectObject ( hMemoryDC, hBitmap );

    BitBlt ( hMemoryDC, 0, 0, w, h, hScreenDC, x, y, SRCCOPY | CAPTUREBLT );
    cv::Mat frame ( h, w, CV_8UC4 );

    BITMAPINFOHEADER bi = { 0 };
    bi.biSize = sizeof ( BITMAPINFOHEADER );
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    GetDIBits ( hMemoryDC, hBitmap, 0, h, frame.data, (BITMAPINFO *)&bi, DIB_RGB_COLORS );

    SelectObject ( hMemoryDC, hOldBitmap );
    DeleteObject ( hBitmap );
    DeleteDC ( hMemoryDC );
    ReleaseDC ( NULL, hScreenDC );

    return frame;
}

void ExecuteScreenCaptureWorkflow () {
    while ( true ) {
        MessageBoxA ( g_hMasterWindow,
            "Instructions:\n\n1. Press OK to freeze your workspace with a selection layer.\n2. Click and drag a marquee box directly around the QR Code to scan it.",
            "Screen Capture Mode", MB_OK | MB_ICONINFORMATION );

        int cx = GetSystemMetrics ( SM_CXSCREEN );
        int cy = GetSystemMetrics ( SM_CYSCREEN );

        g_selection.done = false;

        HWND hOverlay = CreateWindowExA (
            WS_EX_TOPMOST | WS_EX_LAYERED, "QRScreencapOverlayClass", "",
            WS_POPUP | WS_VISIBLE, 0, 0, cx, cy, g_hMasterWindow, NULL, GetModuleHandleA ( NULL ), NULL
        );
        SetLayeredWindowAttributes ( hOverlay, 0, 100, LWA_ALPHA );

        MSG msg;
        while ( GetMessageA ( &msg, NULL, 0, 0 ) ) {
            if ( msg.message == WM_CLOSE && msg.hwnd == hOverlay ) {
                DestroyWindow ( hOverlay );
                break;
            }
            TranslateMessage ( &msg );
            DispatchMessageA ( &msg );
        }

        if ( !g_selection.done ) return;

        int grabX = (std::min)( g_selection.start.x, g_selection.end.x );
        int grabY = (std::min)( g_selection.start.y, g_selection.end.y );
        int grabW = std::abs ( g_selection.start.x - g_selection.end.x );
        int grabH = std::abs ( g_selection.start.y - g_selection.end.y );

        if ( grabW < 5 || grabH < 5 ) {
            MessageBoxA ( g_hMasterWindow, "Selected window capture boundaries are too small.", "Capture Error", MB_OK | MB_ICONERROR );
            continue;
        }

        cv::Mat grabbedFrame = CaptureTargetRegion ( grabX, grabY, grabW, grabH );
// Check the first 5 pixels of the captured frame
        for ( int i = 0; i < 5; i++ ) {
            // OpenCV Vec4b maps bytes sequentially in memory: [0]=B, [1]=G, [2]=R, [3]=A
            cv::Vec4b pixel = grabbedFrame.at<cv::Vec4b> ( 0, i );

            char debugBuf[256];
            wsprintfA ( debugBuf, "Pixel %d -> Memory Byte 0 (B): %d | Memory Byte 1 (G): %d | Memory Byte 2 (R): %d\n",
                i, pixel[0], pixel[1], pixel[2] );
            OutputDebugStringA ( debugBuf ); // Prints to Visual Studio's Output Window or DebugView
        }
        // Right after capturing your screen mat:
        cv::imwrite ( "screengrab.png", grabbedFrame );
/*
        cv::Mat processedFrame = grabbedFrame.clone(); // Fallback default for native Windows

        // Dynamic runtime check: Does the host OS contain Wine's kernel hook?
        HMODULE hWineGetVersion = GetModuleHandleA("ntdll.dll");
        if (hWineGetVersion && GetProcAddress(hWineGetVersion, "wine_get_version")) {
            // We are running on Linux via Wine! Swap the channel byte layout.
            cv::cvtColor(grabbedFrame, processedFrame, cv::COLOR_RGBA2BGRA);
        }
*/
        // ----------------------------------------

        std::string decodedOutput;
/*
        // Feed the dynamically processed frame directly into ZXing
        if (ProcessImageBuffer(processedFrame, decodedOutput, true)) {
            std::string choiceMsg = "Valid QR Code Found!\n\nContent:\n" + decodedOutput + "\n\nChoose an action:";
            int selection = MessageBoxA(g_hMasterWindow, choiceMsg.c_str(), "QR Code Decoded", MB_ABORTRETRYIGNORE | MB_ICONINFORMATION);

            if (selection == IDABORT) break;
            if (selection == IDRETRY) continue;
            break;
        } else {
*/
        if ( ProcessImageBuffer ( grabbedFrame, decodedOutput, true ) ) {
            std::string choiceMsg = "Valid QR Code Found!\n\nContent:\n" + decodedOutput + "\n\nChoose an action:";
            int selection = MessageBoxA ( g_hMasterWindow, choiceMsg.c_str (), "QR Code Decoded", MB_ABORTRETRYIGNORE | MB_ICONINFORMATION );

            if ( selection == IDABORT ) break;
            if ( selection == IDRETRY ) continue;
            break;
        } else {
            int retryResult = MessageBoxA ( g_hMasterWindow,
                "No valid QR Code was discovered inside your selection canvas area.",
                "Scan Evaluation Error", MB_RETRYCANCEL | MB_ICONERROR );
            if ( retryResult != IDRETRY ) break;
        }
    }
}

// --- NATIVE WIN32 CAM RENDERING WINDOW PROCEDURE ---
LRESULT CALLBACK CamWndProc ( HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam ) {
    if ( uMsg == WM_CLOSE ) {
        g_bCameraRunning = false;
        DestroyWindow ( hwnd );
        return 0;
    }
    return DefWindowProcA ( hwnd, uMsg, wParam, lParam );
}

// --- REWRITTEN LIVE HARDWARE CAMERA PIPELINE WORKER THREAD ---
DWORD WINAPI CameraThreadProc ( LPVOID lpParam ) {
    cv::VideoCapture cap ( 0, cv::CAP_DSHOW );
    if ( !cap.isOpened () ) {
        MessageBoxA ( g_hMasterWindow, "Could not map system frame drivers through DirectShow interfaces under local user profile privileges.", "Hardware Configuration Failure", MB_OK | MB_ICONERROR );
        return 0;
    }

    g_bCameraRunning = true;
    cv::Mat frame;

    // Register a native Win32 window class for our live camera preview stream
    HINSTANCE hInst = GetModuleHandleA ( NULL );
    WNDCLASSA camWc = {};
    camWc.lpfnWndProc = CamWndProc;
    camWc.hInstance = hInst;
    camWc.lpszClassName = "LiveCamWinClass";
    camWc.hbrBackground = (HBRUSH)GetStockObject ( BLACK_BRUSH );
    camWc.hCursor = LoadCursorA ( NULL, IDC_ARROW );
    RegisterClassA ( &camWc );

    // Create the native window framework (replaces cv::namedWindow)
    HWND hCamWindow = CreateWindowExA (
        0, "LiveCamWinClass", "Live Scan Pipeline",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        g_hMasterWindow, NULL, hInst, NULL
    );

    ShowWindow ( hCamWindow, SW_SHOW );
    UpdateWindow ( hCamWindow );

    // Prepare Bitmap layout structures for direct GDI screen painting
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof ( BITMAPINFOHEADER );
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24; // BGR format matches OpenCV 8UC3
    bmi.bmiHeader.biCompression = BI_RGB;

    while ( g_bCameraRunning ) {
        cap >> frame;
        if ( frame.empty () ) break;

        // Process standard Win32 message queues so window remains responsive
        MSG msg;
        while ( PeekMessageA ( &msg, hCamWindow, 0, 0, PM_REMOVE ) ) {
            TranslateMessage ( &msg );
            DispatchMessageA ( &msg );
        }

        // Paint the OpenCV frame directly to the window using GDI (replaces cv::imshow)
        HDC hdc = GetDC ( hCamWindow );
        RECT rect;
        GetClientRect ( hCamWindow, &rect );

        bmi.bmiHeader.biWidth = frame.cols;
        bmi.bmiHeader.biHeight = -frame.rows; // Negative for top-down bit array mapping

        StretchDIBits ( hdc, 0, 0, rect.right, rect.bottom,
            0, 0, frame.cols, frame.rows,
            frame.data, &bmi, DIB_RGB_COLORS, SRCCOPY );
        ReleaseDC ( hCamWindow, hdc );

        std::string decodedMsg;
        if ( ProcessImageBuffer ( frame, decodedMsg, false ) ) {
            MessageBeep ( MB_ICONASTERISK );

            // Freeze display window frame update tags (replaces cv::setWindowTitle)
            SetWindowTextA ( hCamWindow, "Live Scan Pipeline - FRAME FROZEN" );

            std::string interactiveLayoutPrompt = "QR Code Found via Camera Stream!\n\nData:\n" + decodedMsg + "\n\nChoose an action:";
            int dynamicChoice = MessageBoxA ( hCamWindow, interactiveLayoutPrompt.c_str (), "Camera Capture Active", MB_ABORTRETRYIGNORE | MB_ICONQUESTION );

            if ( dynamicChoice == IDABORT ) {
                g_bCameraRunning = false;
            } else if ( dynamicChoice == IDRETRY ) {
                SetWindowTextA ( hCamWindow, "Live Scan Pipeline" );
            } else {
                g_bCameraRunning = false;
            }
        }

        // Short sleep invocation to maintain frame execution rhythms (replaces cv::waitKey)
        Sleep ( 30 );
    }

    cap.release ();
    if ( IsWindow ( hCamWindow ) ) {
        DestroyWindow ( hCamWindow );
    }
    UnregisterClassA ( "LiveCamWinClass", hInst );
    return 0;
}
