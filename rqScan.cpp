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
LRESULT CALLBACK CamWndProc ( HWND, UINT, WPARAM, LPARAM );

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
	// Force the OS/Wine loader to trust our application coordinates explicitly
    SetProcessDPIAware(); 
    
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

cv::Mat CaptureTargetRegion(int x, int y, int w, int h) {
    HDC hScreenDC = GetDC(NULL);

    // 1. Fetch GNOME Panel/Dock Offsets dynamically using the active Work Area mapping rules
    RECT workArea = { 0 };
    int offsetX = 0;
    int offsetY = 0;

    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0)) {
        // The Work Area 'left' is the dock width; 'top' is the status bar thickness
        offsetX = workArea.left;
        offsetY = workArea.top;
    }

    // 2. Fall back to standard Windows DPI calculation if running natively on a scaled screen
    double scaleFactor = 1.0;
    int physicalWidth  = GetDeviceCaps(hScreenDC, DESKTOPHORZRES);
    int logicalWidth   = GetDeviceCaps(hScreenDC, HORZRES);
    if (logicalWidth > 0 && physicalWidth != logicalWidth) {
        scaleFactor = (double)physicalWidth / (double)logicalWidth;
    }

        
    FILE *fptr = fopen("scale.txt", "w");
    if (fptr != NULL) {
      	fprintf(fptr, "offsetX: %d\n", offsetX);
  		fprintf(fptr, "offsetY: %d\n", offsetY);
    	fprintf(fptr, "scaleFactor: %.4f\n", scaleFactor);
    	fclose(fptr);
    }


    // 3. Inject the dynamic workspace offsets to correctly align cursor bounds with the physical display
    int realX = (int)((x + offsetX) * scaleFactor);
    int realY = (int)((y + offsetY) * scaleFactor);
    int realW = (int)(w * scaleFactor);
    int realH = (int)(h * scaleFactor);

    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, realW, realH);
    HGDIOBJ hOldBitmap = SelectObject(hMemoryDC, hBitmap);

    // Pass the perfectly realigned coordinates down to the GDI capture array
    BitBlt(hMemoryDC, 0, 0, realW, realH, hScreenDC, realX, realY, SRCCOPY);
    
    cv::Mat frame(realH, realW, CV_8UC4);

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = realW;
    bi.biHeight = -realH;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    GetDIBits(hMemoryDC, hBitmap, 0, realH, frame.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(hMemoryDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(NULL, hScreenDC);

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

        if ( !g_selection.done ) return; // User aborted via ESC key hook

        int grabX = (std::min)( g_selection.start.x, g_selection.end.x );
        int grabY = (std::min)( g_selection.start.y, g_selection.end.y );
        int grabW = std::abs ( g_selection.start.x - g_selection.end.x );
        int grabH = std::abs ( g_selection.start.y - g_selection.end.y );

        if ( grabW < 5 || grabH < 5 ) {
            MessageBoxA ( g_hMasterWindow, "Selected window capture boundaries are too small.", "Capture Error", MB_OK | MB_ICONERROR );
            continue;
        }

        // --- THE UNIVERSAL TRANSPARENCY BYPASS FIX ---
        // 1. Hide the semi-transparent black overlay veil window instantly
        ShowWindow ( hOverlay, SW_HIDE );

        // 2. Force an immediate system repaint loop so the desktop underneath refreshes on screen
        UpdateWindow ( GetDesktopWindow () );
        Sleep ( 50 ); // Give the X11 server 50ms to clear the overlay graphic surface

        // 3. Grab the raw desktop pixels now that the overlay barrier is completely gone
        cv::Mat grabbedFrame = CaptureTargetRegion ( grabX, grabY, grabW, grabH );

        // 4. Destroy the overlay window since the selection action is completely finished
        DestroyWindow ( hOverlay );
        // ----------------------------------------------

        // --- IN-APP SCREEN CAPTURE VISUAL PREVIEW WINDOW ---
        HINSTANCE hInst = GetModuleHandleA ( NULL );
        WNDCLASSA previewWc = {};
        // Reuse our CamWndProc loop since it already knows how to handle a clean close event
        previewWc.lpfnWndProc = CamWndProc;
        previewWc.hInstance = hInst;
        previewWc.lpszClassName = "ScreenPreviewWinClass";
        previewWc.hbrBackground = (HBRUSH)GetStockObject ( BLACK_BRUSH );
        previewWc.hCursor = LoadCursorA ( NULL, IDC_ARROW );
        RegisterClassA ( &previewWc );

        // PIXEL-PERFECT VISUAL FIX: Calculate exact outer dimensions needed to clear borders & titlebar
        RECT winRect = { 0, 0, grabbedFrame.cols, grabbedFrame.rows };
        DWORD winStyle = WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX;
        AdjustWindowRect ( &winRect, winStyle, FALSE );

        int outerWidth = winRect.right - winRect.left;
        int outerHeight = winRect.bottom - winRect.top;

        // Create a dedicated pop-up window sized perfectly using corrected dimensions
        HWND hPreviewWin = CreateWindowExA (
            WS_EX_TOPMOST, "ScreenPreviewWinClass", "Captured Frame Debug Preview",
            winStyle,
            CW_USEDEFAULT, CW_USEDEFAULT, outerWidth, outerHeight,
            g_hMasterWindow, NULL, hInst, NULL
        );

        ShowWindow ( hPreviewWin, SW_SHOW );
        UpdateWindow ( hPreviewWin );

        // Prepare 32-bit Bitmap structure info (Matches our CV_8UC4 screen capture grab matrix)
        BITMAPINFO previewBmi = { 0 };
        previewBmi.bmiHeader.biSize = sizeof ( BITMAPINFOHEADER );
        previewBmi.bmiHeader.biWidth = grabbedFrame.cols;
        previewBmi.bmiHeader.biHeight = -grabbedFrame.rows; // Negative for top-down bit mapping
        previewBmi.bmiHeader.biPlanes = 1;
        previewBmi.bmiHeader.biBitCount = 32; // 32-bit for BGRA screen captures
        previewBmi.bmiHeader.biCompression = BI_RGB;

        // Force a window message pump loop to draw the pixels visually onto the screen surface
        HDC hPreviewDC = GetDC ( hPreviewWin );
        RECT previewRect;
        GetClientRect ( hPreviewWin, &previewRect );

        StretchDIBits ( hPreviewDC, 0, 0, previewRect.right, previewRect.bottom,
            0, 0, grabbedFrame.cols, grabbedFrame.rows,
            grabbedFrame.data, &previewBmi, DIB_RGB_COLORS, SRCCOPY );
        ReleaseDC ( hPreviewWin, hPreviewDC );
        // ---------------------------------------------------------

        // Dynamic runtime check: Adjust color channels for Wine if running on Linux
        cv::Mat processedFrame = grabbedFrame.clone ();
        HMODULE hWineGetVersion = GetModuleHandleA ( "ntdll.dll" );
        if ( hWineGetVersion && GetProcAddress ( hWineGetVersion, "wine_get_version" ) ) {
            cv::cvtColor ( grabbedFrame, processedFrame, cv::COLOR_RGBA2BGRA );
        }

        // Optional Debug: Still write to disk if needed
        cv::imwrite ( "screengrab.png", processedFrame );

        std::string decodedOutput;
        if ( ProcessImageBuffer ( processedFrame, decodedOutput, true ) ) {
            // Close the preview window explicitly right before displaying the success message box
            if ( IsWindow ( hPreviewWin ) ) DestroyWindow ( hPreviewWin );
            UnregisterClassA ( "ScreenPreviewWinClass", hInst );

            std::string choiceMsg = "Valid QR Code Found!\n\nContent:\n" + decodedOutput + "\n\nChoose an action:";
            int selection = MessageBoxA ( g_hMasterWindow, choiceMsg.c_str (), "QR Code Decoded", MB_ABORTRETRYIGNORE | MB_ICONINFORMATION );

            if ( selection == IDABORT ) break;
            if ( selection == IDRETRY ) continue;
            break;
        } else {
            // Leave the preview window open so the user can look at it alongside the error message box
            int retryResult = MessageBoxA ( g_hMasterWindow,
                "No valid QR Code was discovered inside your selection canvas area.\n\nReview the open preview window to check the captured area.",
                "Scan Evaluation Error", MB_RETRYCANCEL | MB_ICONERROR );

            // Clean up the preview window when they click Retry or Cancel
            if ( IsWindow ( hPreviewWin ) ) DestroyWindow ( hPreviewWin );
            UnregisterClassA ( "ScreenPreviewWinClass", hInst );

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
