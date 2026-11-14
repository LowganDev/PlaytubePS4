#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/result.hpp"
#include "ui/box.hpp"
#include "ui/text.hpp"
#include "ui/svg_shapes.hpp"
#include "ui/view.hpp"

namespace playtube::app {

// Text styles and paddings shared by the screens.
ui::FontStyle bodyText();
ui::FontStyle bodyStrong();
ui::FontStyle headingText();
ui::FontStyle titleText(ui::FontWeight weight);
ui::Insets rowPadding();   // a one-line row
ui::Insets pagePadding();  // a scrolling page under the navigation bar

// "1:05", "1:02:26" (negative: "0:00").
std::string clockText(double seconds);

// What the user reads for a failed request.
std::string errorText(const core::Error& error);

// A message under the content (secondary colour) with an optional button ("Réessayer"); returns its box.
ui::Box* addMessage(ui::Box& column, const std::string& text, const std::string& buttonText = {},
                    std::function<void()> onPress = {});

// First code point of a name, upper-cased for ASCII letters ("?" when empty).
std::string initialOf(const std::string& name);

// "Playtube." with the dot in the accent colour; the picture given to setLogoPicture instead while there is one
// (the YouTube logo, when the user chose it in the settings). Every logo shown changes at once.
std::unique_ptr<ui::View> makeLogo();
void setLogoPicture(std::shared_ptr<const ui::SvgPicture> picture);
bool logoPictureShown();

// A 48 px circle in the accent colour with the user's initial.
std::unique_ptr<ui::View> makeAvatar(const std::string& userName);

// The background colour fading over a backdrop: opaque on the bottom 30 %, 56 % at the top (the backdrop shows
// as through 55 % opacity and a 20 % shade, while drawn opaque: the renderer copies it).
class BackdropShade : public ui::View {
public:
    BackdropShade();
    void draw(NVGcontext* vg) const override;
};

}  // namespace playtube::app
