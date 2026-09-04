//
// imgui_caret - does a text box own the caret, this frame?
//
// RENDER THREAD ONLY: it reads the ImGui context. The full map asks it directly (its own
// bare keys - WASD, E/Q, Home, F, Space - belong to the search box while one is up), and
// the frame publishes the answer to the loop thread, whose GetAsyncKeyState hotkeys have
// no other way to know.
//

#pragma once

#include "imgui.h"
#include "imgui_internal.h"

namespace tgate
{
    // `io.WantTextInput` alone is a frame behind the caret: NewFrame() computes it from
    // what EndFrame() saw LAST frame, so on the frame a click activates a box it is
    // still false - and a key sampled inside that window counts as a binding. The other
    // two terms are this frame's own answer: `WantTextInputNextFrame` is what EndFrame()
    // has just recorded, and the ActiveId test holds from the moment the widget takes
    // the caret, before any EndFrame has run for it at all.
    inline bool text_active()
    {
        const ImGuiContext* g = ImGui::GetCurrentContext();
        if (g == nullptr)
        {
            return false;
        }
        if (g->IO.WantTextInput || g->WantTextInputNextFrame == 1)
        {
            return true;
        }
        // The match, not just a non-zero id: InputTextState.ID outlives the widget it
        // belonged to, and equals ActiveId only while that widget is the active one.
        return g->ActiveId != 0 && g->ActiveId == g->InputTextState.ID;
    }
} // namespace tgate
