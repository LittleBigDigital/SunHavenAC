#include "RunApp.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>

#ifndef kVK_RightShift
#define kVK_RightShift 0x3C
#endif
#ifndef kVK_ANSI_R
#define kVK_ANSI_R 0x0F
#endif
#ifndef kVK_ForwardDelete
#define kVK_ForwardDelete 0x75
#endif
#ifndef kVK_ANSI_Z
#define kVK_ANSI_Z 0x06
#endif
#ifndef kVK_ANSI_X
#define kVK_ANSI_X 0x07
#endif

static volatile bool gAssignedTriggerDown = false;
static pthread_t gWorkerThread;
static volatile bool gWorkerRunning = false;
static volatile CFTimeInterval gRepeatInterval = 0.2; // default 200ms

static volatile bool gUseMouseTrigger = true; // true: mouse, false: key
static volatile int gTriggerMouseButton = 2;  // 0=left, 1=right, 2=middle, others as reported
static volatile uint16_t gTriggerKeyCode = 0; // Carbon virtual key code
static volatile uint64_t gTriggerModifierMask = 0; // NSEventModifierFlags mask subset (ctrl/cmd/opt/shift/fn)
static volatile bool gAwaitingKeyUp = false; // for key trigger, track key up to stop worker

static CFMachPortRef gEventTap = NULL;
static CFRunLoopSourceRef gRunLoopSource = NULL;
static CFRunLoopRef gRunLoop = NULL;

static volatile bool ignoreInitialRelease = false;

static volatile sig_atomic_t gShouldQuit = 0;

static uint64_t compactModifierMask(CGEventFlags flags) {
    uint64_t m = 0;
    if (flags & kCGEventFlagMaskControl) m |= (1ULL << 0);
    if (flags & kCGEventFlagMaskCommand) m |= (1ULL << 1);
    if (flags & kCGEventFlagMaskAlternate) m |= (1ULL << 2);
    if (flags & kCGEventFlagMaskShift) m |= (1ULL << 3);
#ifdef kCGEventFlagMaskSecondaryFn
    if (flags & kCGEventFlagMaskSecondaryFn) m |= (1ULL << 4);
#endif
    return m;
}

static inline uint64_t maskIgnoringOption(uint64_t compactMask) {
    // bit 2 corresponds to Alternate (Option) in compact mask
    const uint64_t optionBit = (1ULL << 2);
    return (compactMask & ~optionBit);
}

//This code will press Z, wait 10ms, then press X
static void cancelAnimation(void){
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
    

    // Press X
    usleep(10000); // 10ms pause between Z and X
    CGEventRef keyEventXDown = CGEventCreateKeyboardEvent(source, kVK_ANSI_X, true);
    CGEventRef keyEventXUp = CGEventCreateKeyboardEvent(source, kVK_ANSI_X, false);
    CGEventPost(kCGSessionEventTap, keyEventXDown);
    usleep(10000);
    CGEventPost(kCGSessionEventTap, keyEventXUp);
    
    usleep(20000);
    
    // Press Z
    CGEventRef keyEventZDown = CGEventCreateKeyboardEvent(source, kVK_ANSI_Z, true);
    CGEventRef keyEventZUp = CGEventCreateKeyboardEvent(source, kVK_ANSI_Z, false);
    CGEventPost(kCGSessionEventTap, keyEventZDown);
    usleep(10000);
    CGEventPost(kCGSessionEventTap, keyEventZUp);
    
    usleep(20000);

    CFRelease(keyEventZDown);
    CFRelease(keyEventZUp);
    CFRelease(keyEventXDown);
    CFRelease(keyEventXUp);
    CFRelease(source);
}

static void synthesizeLeftClick(void) {
    CGEventRef current = CGEventCreate(NULL);
    CGPoint location = CGEventGetLocation(current);
    CFRelease(current);
    CGEventRef mouseDown = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseDown, location, kCGMouseButtonLeft);
    CGEventRef mouseUp = CGEventCreateMouseEvent(NULL, kCGEventLeftMouseUp, location, kCGMouseButtonLeft);
    if (mouseDown) CGEventPost(kCGHIDEventTap, mouseDown);
    usleep(20000);
    if (mouseUp) CGEventPost(kCGHIDEventTap, mouseUp);
    if (mouseDown) CFRelease(mouseDown);
    if (mouseUp) CFRelease(mouseUp);
}

static void *CancelAnimationWorker(void *arg) {
    (void)arg;
    while (1) {
        synthesizeLeftClick();
        useconds_t delayUs = (useconds_t)(gRepeatInterval * 1000000.0);
        
        CGEventRef flagsEvent = CGEventCreate(NULL);

        if (flagsEvent) {
            CGEventFlags flags = CGEventGetFlags(flagsEvent);
            if (flags & kCGEventFlagMaskAlternate) {
                delayUs *= 2; // double to 200ms when Option is held
            }
            CFRelease(flagsEvent);
        }
        usleep(delayUs);
        cancelAnimation();
        usleep(0.1 * 1000000.0);
        if (!gAssignedTriggerDown || gShouldQuit) {
            break;
        }
    }
    gWorkerRunning = false;
    return NULL;
}

static CGEventRef CGEventCallback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *refcon) {
    (void)proxy; (void)refcon;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (gEventTap) CGEventTapEnable(gEventTap, true);
        return event;
    }

    // Mouse trigger handling
    if (gUseMouseTrigger) {
        if (type == kCGEventRightMouseDown || type == kCGEventOtherMouseDown) {
            //ignoreInitialRelease = true;
    
            // Determine button index
            int button = 0;
            if (type == kCGEventRightMouseDown) button = 1;
            else button = (int)CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            // Ignore left if configured to avoid left clicks as trigger
            // (Caller should not set 0; this just safety-checks.)
            if (button == 0) return event;
            uint64_t mods = compactModifierMask(CGEventGetFlags(event));
            // Allow trigger to match even if Option is additionally held down
            if (button == gTriggerMouseButton && maskIgnoringOption(mods) == maskIgnoringOption(gTriggerModifierMask)) {
                gAssignedTriggerDown = true;
                if (!gWorkerRunning) {
                    gWorkerRunning = true;
                    pthread_create(&gWorkerThread, NULL, CancelAnimationWorker, NULL);
                    pthread_detach(gWorkerThread);
                }
            }
            return event;
        }
        
        if ((type == kCGEventRightMouseUp || type == kCGEventOtherMouseUp) && !ignoreInitialRelease) {
            int button = (type == kCGEventRightMouseUp) ? 1 : (int)CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            if (button == gTriggerMouseButton && button != 0) {
                gAssignedTriggerDown = false;
            }
            return event;
        }

        return event;
    }

    // Key trigger handling
    if (type == kCGEventKeyDown) {
        uint16_t keyCode = (uint16_t)CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
        uint64_t mods = compactModifierMask(CGEventGetFlags(event));
        // Allow trigger to match even if Option is additionally held down
        if (keyCode == gTriggerKeyCode && maskIgnoringOption(mods) == maskIgnoringOption(gTriggerModifierMask)) {
            gAssignedTriggerDown = true;
            gAwaitingKeyUp = true;
            if (!gWorkerRunning) {
                gWorkerRunning = true;
                pthread_create(&gWorkerThread, NULL, CancelAnimationWorker, NULL);
                pthread_detach(gWorkerThread);
            }
        }
        return event;
    }
    
    if (type == kCGEventKeyUp) {
        if (gAwaitingKeyUp) {
            gAssignedTriggerDown = false;
            gAwaitingKeyUp = false;
        }
        return event;
    }
    
    // Stop mouse-trigger repeat when required modifiers are released
//    if (type == kCGEventFlagsChanged && gUseMouseTrigger && gAssignedTriggerDown) {
//        uint64_t mods = compactModifierMask(CGEventGetFlags(event));
//        if (maskIgnoringOption(mods) != maskIgnoringOption(gTriggerModifierMask)) {
//            gAssignedTriggerDown = false;
//        }
//        return event;
//    }

    return event;
}

static void startEventTap(void) {
    if (gEventTap) return;
    CGEventMask eventMask = CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) | CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);
    gEventTap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, 0, eventMask, CGEventCallback, NULL);
    if (!gEventTap) {
        fprintf(stderr, "Accessibility permissions not granted. Enable in System Settings → Privacy & Security → Accessibility.\n");
        // Do not exit; allow caller to decide. Just return without setting quit flag.
        return;
    }
    gRunLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, gEventTap, 0);
    gRunLoop = CFRunLoopGetCurrent();
    CFRetain(gRunLoop);
    CFRunLoopAddSource(gRunLoop, gRunLoopSource, kCFRunLoopCommonModes);
    CGEventTapEnable(gEventTap, true);
}

static void stopEventTapInternal(void) {
    if (gEventTap) {
        CGEventTapEnable(gEventTap, false);
    }
    if (gRunLoopSource) {
        if (gRunLoop) CFRunLoopRemoveSource(gRunLoop, gRunLoopSource, kCFRunLoopCommonModes);
        CFRelease(gRunLoopSource);
        gRunLoopSource = NULL;
    }
    if (gEventTap) {
        CFRelease(gEventTap);
        gEventTap = NULL;
    }
    if (gRunLoop) {
        CFRelease(gRunLoop);
        gRunLoop = NULL;
    }
}

void runApp(void) {
    gShouldQuit = 0;
    gAssignedTriggerDown = false;

    startEventTap();
    if (gShouldQuit) {
        // Failed to start; clean any partial state
        stopEventTapInternal();
        return;
    }

    // Run the CFRunLoop until stopApp() is called
    while (!gShouldQuit) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
    }

    // Clean up
    gAssignedTriggerDown = false;
    stopEventTapInternal();
    // Give worker thread a moment to observe gAssignedTriggerDown = false
    usleep(50000);
}

void stopApp(void) {
    gShouldQuit = 1;
}

bool isAccessibilityEnabled(void) {
    // Returns true if the app has Accessibility permissions
    return AXIsProcessTrusted();
}

void setRepeatIntervalMilliseconds(int milliseconds) {
    if (milliseconds < 1) milliseconds = 1;
    if (milliseconds > 500) milliseconds = 500;
    gRepeatInterval = ((CFTimeInterval)milliseconds) / 1000.0; // seconds
}

void setMouseTrigger(int buttonNumber, uint64_t modifiersMask) {
    if (buttonNumber < 0) buttonNumber = 0;
    gUseMouseTrigger = true;
    gTriggerMouseButton = buttonNumber;
    gTriggerModifierMask = modifiersMask;
}

void setKeyTrigger(uint16_t keyCode, uint64_t modifiersMask) {
    gUseMouseTrigger = false;
    gTriggerKeyCode = keyCode;
    gTriggerModifierMask = modifiersMask;
}

