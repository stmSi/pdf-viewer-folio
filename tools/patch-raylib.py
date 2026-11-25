"""Apply the viewer's Android input and native surface fixes to pinned raylib 6.2."""
from pathlib import Path

source = Path('.deps/raylib-6.2/src/platforms/rcore_android.c')
text = source.read_text()
before = text


def replace(original, fixed):
    global text
    if fixed in text:
        return
    if original in text:
        if text.count(original) != 1:
            raise SystemExit('Ambiguous raylib Android patch context.')
        text = text.replace(original, fixed, 1)
    else:
        raise SystemExit('Could not apply a raylib Android patch; check the pinned source.')


# Arrow key constants are 262-265, beyond the backend's old snapshot loop.
replace('    // Register previous keys states\n'
        '    // NOTE: Android supports up to 260 keys\n'
        '    for (int i = 0; i < 260; i++)',
        '    // Register previous keys states\n'
        '    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++)')

# Folio uses the full native surface. Fixed buffer dimensions stretch the initial
# portrait framebuffer after rotation and leave input and menu geometry out of sync.
replace('ANativeWindow_setBuffersGeometry(platform.app->window, CORE.Window.render.width, CORE.Window.render.height, displayFormat);',
        'ANativeWindow_setBuffersGeometry(platform.app->window, 0, 0, displayFormat);')
replace('''ANativeWindow_setBuffersGeometry(app->window,
                        CORE.Window.render.width + CORE.Window.renderOffset.x,
                        CORE.Window.render.height + CORE.Window.renderOffset.y,
                        displayFormat);''',
        'ANativeWindow_setBuffersGeometry(app->window, 0, 0, displayFormat);')

replace('void PollInputEvents(void)\n{\n#if SUPPORT_GESTURES_SYSTEM',
        'void PollInputEvents(void)\n{\n    CORE.Window.resizedLastFrame = false;\n#if SUPPORT_GESTURES_SYSTEM')

marker = '''    }
}

//----------------------------------------------------------------------------------
// Module Internal Functions Definition'''
resize = '''    }

    // Folio: match the viewport and input coordinates to the native surface.
    // Polling also covers resize events delivered after CONFIG_CHANGED.
    if (CORE.Window.ready && !platform.contextRebindRequired && platform.app->window != NULL)
    {
        int width = ANativeWindow_getWidth(platform.app->window);
        int height = ANativeWindow_getHeight(platform.app->window);
        if (width > 0 && height > 0 &&
            (width != CORE.Window.screen.width || height != CORE.Window.screen.height))
        {
            CORE.Window.screen = (Size){ width, height };
            CORE.Window.display = CORE.Window.screen;
            CORE.Window.renderOffset = (Point){ 0, 0 };
            CORE.Window.screenScale = MatrixIdentity();
            SetupViewport(width, height);
            CORE.Window.currentFbo = CORE.Window.render;
            CORE.Window.resizedLastFrame = true;
        }
    }
}

//----------------------------------------------------------------------------------
// Module Internal Functions Definition'''
replace(marker, resize)

if text != before:
    source.write_text(text)
