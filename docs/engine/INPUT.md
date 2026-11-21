# Input

How a key, a click or a mouse move gets from the OS to a GUI element and to a
script, in the shipped client and in a Studio viewport.

## The path

```
                     SDL event (key, mouse, wheel, text, pad)
                                     |
           +-------------------------+--------------------------+
           |  client                                            |  Studio
           v                                                    v
   input::Translator                                  ImGui io          input::Translator
   (Client::Input)                                    (keys, mouse,     (PlayedInput)
     |  State(): keys held, mouse, buttons             text, pads)        |  State(): every key held
     |  PendingPresentation(): ordered key presses,      |                |
     |    text edits, click samples, wheel               |                |
     |                                                   |                |
     |                 one canvas per frame              |                |
     v                                                   v                |
  +----------------------------------------------------------------+     |
  |  client::GuiFrameInput      (mono.client/include/client/GuiInput.hpp)  |
  |    Pointer (level)   Samples   KeyPresses   Controllers   Edits  |     |
  +----------------------------------------------------------------+     |
     |                                                                    |
     v                                                                    |
  RouteGuiKeyboard         key and pad navigation, then typing            |
  RouteGuiPointerFrame     each click sample, then the pointer level      |
     |   (RouteGuiFrame = both, in that order)                            |
     v                                                                    |
  gui::Router  ->  hover, press, Activated, InputBegan, TextBox focus     |
     |                                                                    |
     v                                                                    |
  GuiFrameEvents (Typed, Semantic, Pointer)                               |
     |                                                                    |
     v                                                                    v
  Runtime::DeliverGuiEvents  (queued)        scene::InputState in the world
     |                                         client: WriteInput
     |                                         Studio: DrivePlayer (selected viewport only)
     v                                                    |
  next tick's Heartbeat  ------------------------------->  PumpInput
     GuiButton.Activated, MouseEnter ...                    UserInputService.InputBegan / Ended,
                                                            InputChanged, bound actions
```

## Rules

- Hover follows the mouse into any Studio viewport. It needs no click first.
- A press, keys and typing go to the selected viewport only. Clicking a
  viewport selects it in the same frame as the press.
- ImGui chrome over a viewport takes the pointer away from the game GUI.
- The GUI list is compiled with the previous frame's hover and press, so a
  shade change is drawn one frame after the move that caused it. A viewport
  whose list changed under interaction is drawn next, ahead of the panels'
  turn order.
- Typing reaches a `TextBox` only when the viewport shows it. Escape cancels
  only such a box.
- Every key reaches scripts. Studio copies the translator's keys into the
  played world; it does not filter them.
- Pointer routing happens on the frame the input arrives, against the list
  compiled that frame. Nothing is queued against an older frame.

## Where the code is

| Step | Client | Studio |
|---|---|---|
| SDL to translator | `Client::PumpEvents` | `Editor::PumpEvents` |
| Frame input for the GUI | `Client.cpp`, presentation step | `Editor::DrawViewportGui` (pointer), `Editor::RouteViewportGuiKeyboard` (keys, from ImGui) |
| Routing | `client::RouteGuiFrame` | `client::RouteGuiPointerFrame`, `client::RouteGuiKeyboard` |
| Keys to the world | `Client::WriteInput` | `Editor::DrivePlayer` |
| Script signals | `PumpInput`, `PumpGuiEvents` in the script runtime | same |

Studio reads its keyboard and text from ImGui, because ImGui owns SDL text
input there and spreads a fast press over two frames. The client reads its
translator. Both build the same `GuiFrameInput` and route through the same
functions.
