#include "monitor/tui/quit_events.h"

#include <gtest/gtest.h>

namespace aquila::monitor {
namespace {

TEST(TuiQuitEventsTest, AcceptsCommonQuitKeys) {
  EXPECT_TRUE(IsQuitEvent(ftxui::Event::q));
  EXPECT_TRUE(IsQuitEvent(ftxui::Event::Q));
  EXPECT_TRUE(IsQuitEvent(ftxui::Event::Character('q')));
  EXPECT_TRUE(IsQuitEvent(ftxui::Event::Escape));
  EXPECT_TRUE(IsQuitEvent(ftxui::Event::CtrlC));
  EXPECT_TRUE(IsQuitEvent(ftxui::Event::CtrlQ));
}

TEST(TuiQuitEventsTest, RejectsNonQuitKeys) {
  EXPECT_FALSE(IsQuitEvent(ftxui::Event::r));
  EXPECT_FALSE(IsQuitEvent(ftxui::Event::ArrowDown));
  EXPECT_FALSE(IsQuitEvent(ftxui::Event::Tab));
}

}  // namespace
}  // namespace aquila::monitor
