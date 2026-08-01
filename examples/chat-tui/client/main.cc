//
//    Copyright 2025 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Chat room client with an FTXUI interface.
//
// The interesting part is the seam between two event loops. FTXUI owns the main
// thread and redraws when told to; znet delivers packets on the client's loop
// thread. Neither may touch the other's state directly, so everything crossing
// over goes through UiModel, a plain struct behind a mutex, and the network
// side wakes the UI with PostEvent(). That is the whole integration: no
// polling, no sleeping, no shared containers touched from two threads.
//
// Names are not chosen here. The server assigns one at connect and sends it in
// a WelcomePacket, which is what keeps them unique.
//
// Run the server first, then one client per terminal:
//   ./chat-tui-server
//   ./chat-tui-client
//

#include "znet/client.h"
#include "znet/client_events.h"
#include "znet/codec.h"
#include "znet/init.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"

#include "packets.h"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace znet;

namespace {

constexpr size_t kScrollback = 500;

struct Line {
  enum class Kind { kChat, kSystem, kSelf };

  Kind kind = Kind::kChat;
  std::string author;
  std::string text;
};

// Everything both threads can see. Every member is guarded by `mutex`.
struct UiModel {
  std::mutex mutex;

  std::vector<std::string> rooms;
  std::string current_room;
  // Assigned by the server, so it is not known until the welcome arrives. Kept
  // here rather than in a bare global because the render thread reads it.
  std::string self_name;
  // One scrollback per room, so switching away and back keeps the history.
  std::map<std::string, std::deque<Line>> history;

  bool connected = false;
  std::string status = "connecting...";

  void Append(const std::string& room, Line line) {
    std::deque<Line>& lines = history[room];
    lines.push_back(std::move(line));
    while (lines.size() > kScrollback) {
      lines.pop_front();
    }
  }
};

UiModel g_model;
ftxui::ScreenInteractive* g_screen = nullptr;
std::shared_ptr<PeerSession> g_session;

// Called from the znet loop thread once the model has been updated. PostEvent
// is the only ScreenInteractive method safe to call from another thread.
void RequestRedraw() {
  if (g_screen != nullptr) {
    g_screen->PostEvent(ftxui::Event::Custom);
  }
}

class ChatHandler : public PacketHandler<ChatHandler, WelcomePacket,
                                         ChatPacket, SystemPacket> {
 public:
  void OnPacket(std::shared_ptr<WelcomePacket> packet) {
    {
      std::lock_guard<std::mutex> lock(g_model.mutex);
      g_model.self_name = packet->name;
      g_model.rooms = packet->rooms;
      g_model.status = "you are " + packet->name;
      if (g_model.current_room.empty() && !g_model.rooms.empty()) {
        g_model.current_room = g_model.rooms.front();
      }
    }
    RequestRedraw();
  }

  void OnPacket(std::shared_ptr<ChatPacket> packet) {
    {
      std::lock_guard<std::mutex> lock(g_model.mutex);
      Line line;
      // The server assigned the name and stamps every message with it, so this
      // comparison is reliable rather than a guess about who we are.
      line.kind = packet->author == g_model.self_name ? Line::Kind::kSelf
                                                      : Line::Kind::kChat;
      line.author = packet->author;
      line.text = packet->text;
      g_model.Append(packet->room, std::move(line));
    }
    RequestRedraw();
  }

  void OnPacket(std::shared_ptr<SystemPacket> packet) {
    {
      std::lock_guard<std::mutex> lock(g_model.mutex);
      Line line;
      line.kind = Line::Kind::kSystem;
      line.text = packet->text;
      g_model.Append(packet->room, std::move(line));
    }
    RequestRedraw();
  }
};

bool OnConnected(ClientConnectedToServerEvent& event) {
  event.session()->SetCodec(MakeCodec());
  event.session()->SetHandler(std::make_shared<ChatHandler>());
  g_session = event.session();

  // Nothing to send: the server names us and follows with a WelcomePacket.
  {
    std::lock_guard<std::mutex> lock(g_model.mutex);
    g_model.connected = true;
    g_model.status = "waiting for a name...";
  }
  RequestRedraw();
  return false;
}

bool OnDisconnected(ClientDisconnectedFromServerEvent& event) {
  (void)event;
  {
    std::lock_guard<std::mutex> lock(g_model.mutex);
    g_model.connected = false;
    g_model.status = "disconnected";
  }
  RequestRedraw();
  return false;
}

// znet::Event, not ftxui::Event. Both names exist here, so this one is spelled
// out rather than pulled in with a using-directive.
void OnNetworkEvent(znet::Event& event) {
  EventDispatcher dispatcher{event};
  dispatcher.Dispatch<ClientConnectedToServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnConnected));
  dispatcher.Dispatch<ClientDisconnectedFromServerEvent>(
      ZNET_BIND_GLOBAL_FN(OnDisconnected));
}

ftxui::Element RenderLine(const Line& line) {
  using namespace ftxui;
  switch (line.kind) {
    case Line::Kind::kSystem:
      return hbox({
          text("   * ") | color(Color::GrayDark),
          paragraph(line.text) | color(Color::GrayDark) | flex,
      });
    case Line::Kind::kSelf:
      return hbox({
          text(" " + line.author + " ") | bold | color(Color::Black) |
              bgcolor(Color::CyanLight),
          text(" "),
          paragraph(line.text) | flex,
      });
    case Line::Kind::kChat:
    default:
      return hbox({
          text(" " + line.author + " ") | bold | color(Color::Black) |
              bgcolor(Color::GreenLight),
          text(" "),
          paragraph(line.text) | flex,
      });
  }
}

}  // namespace

int main(int argc, char** argv) {
  // znet logs to std::cout by default, and so does FTXUI, so a log line lands
  // in the middle of the interface. Redirecting std::cout would take the TUI
  // with it, since FTXUI writes there too; SetLogStream moves only the logging.
  // Do it before Init() so even the startup banner goes to the file.
  // The pid keeps two clients in the same directory from truncating each
  // other's log.
  static std::ofstream log_file("chat-tui-client-" +
                                std::to_string(
#ifdef _WIN32
                                    static_cast<long>(_getpid())
#else
                                    static_cast<long>(getpid())
#endif
                                        ) +
                                ".log");
  znet::SetLogStream(log_file);

  Result result;
  if ((result = znet::Init()) != Result::Success) {
    std::cerr << "Failed to initialize znet: " << GetResultString(result)
              << std::endl;
    return 1;
  }

  // Optional port, matching the server's.
  PortNumber port = 25000;
  if (argc > 1) {
    port = static_cast<PortNumber>(std::atoi(argv[1]));
  }

  ClientConfig config{"localhost", port, std::chrono::seconds(10)};
  Client client{config};
  client.SetEventCallback(ZNET_BIND_GLOBAL_FN(OnNetworkEvent));

  if ((result = client.Bind()) != Result::Success) {
    std::cerr << "Failed to bind: " << GetResultString(result) << std::endl;
    return 1;
  }
  if ((result = client.Connect()) != Result::Success) {
    std::cerr << "Failed to connect: " << GetResultString(result) << std::endl;
    return 1;
  }

  {
    using namespace ftxui;

    auto screen = ScreenInteractive::Fullscreen();
    g_screen = &screen;

    // --- room list ---------------------------------------------------------
    std::vector<std::string> room_entries;
    int selected_room = 0;

    MenuOption menu_option = MenuOption::Vertical();
    menu_option.on_change = [&] {
      std::string room;
      {
        std::lock_guard<std::mutex> lock(g_model.mutex);
        if (selected_room < 0 ||
            selected_room >= static_cast<int>(g_model.rooms.size())) {
          return;
        }
        room = g_model.rooms[static_cast<size_t>(selected_room)];
        if (room == g_model.current_room) {
          return;
        }
        g_model.current_room = room;
      }
      if (g_session) {
        auto select = std::make_shared<SelectRoomPacket>();
        select->room = room;
        g_session->SendPacket(select);
      }
    };
    Component room_menu = Menu(&room_entries, &selected_room, menu_option);

    // --- composer ----------------------------------------------------------
    std::string draft;
    InputOption input_option;
    input_option.multiline = false;
    input_option.on_enter = [&] {
      if (draft.empty() || !g_session) {
        return;
      }
      auto message = std::make_shared<ChatPacket>();
      message->text = draft;  // author and room are the server's to fill in
      g_session->SendPacket(message);
      draft.clear();
    };
    Component input = Input(&draft, "say something", input_option);

    // Focus the composer, not the room list: typing should work the moment the
    // client opens. Tab moves to the sidebar and back.
    int focused_pane = 1;
    Component layout =
        Container::Horizontal({room_menu, input}, &focused_pane);

    Component ui = Renderer(layout, [&] {
      std::vector<std::string> rooms;
      std::string current;
      std::vector<Line> lines;
      bool connected = false;
      std::string status;

      // One short critical section per frame: copy what this frame needs, then
      // render without the lock, so the network thread never waits on drawing.
      {
        std::lock_guard<std::mutex> lock(g_model.mutex);
        rooms = g_model.rooms;
        current = g_model.current_room;
        connected = g_model.connected;
        status = g_model.status;
        auto it = g_model.history.find(current);
        if (it != g_model.history.end()) {
          lines.assign(it->second.begin(), it->second.end());
        }
      }

      // Keep the menu's entries in step with the server's room list.
      room_entries = rooms;

      Elements rendered;
      for (const Line& line : lines) {
        rendered.push_back(RenderLine(line));
      }
      if (rendered.empty()) {
        rendered.push_back(text("   nothing here yet") | color(Color::GrayDark));
      }

      // filler() first pushes a short conversation to the bottom of the pane,
      // the way a chat window reads; focusPositionRelative pins the viewport to
      // the bottom once there is more history than fits, so the newest message
      // is always the visible one.
      Elements stacked;
      stacked.push_back(filler());
      for (Element& line : rendered) {
        stacked.push_back(std::move(line));
      }
      Element messages =
          vbox(std::move(stacked)) | focusPositionRelative(0, 1) | frame | flex;

      Element sidebar = vbox({
                            text(" rooms") | bold | color(Color::GrayLight),
                            separator(),
                            room_menu->Render() | flex,
                        }) |
                        size(WIDTH, EQUAL, 16);

      Element conversation =
          vbox({
              hbox({
                  text(" " +
                       (current.empty() ? std::string("no room") : current)) |
                      bold,
                  filler(),
                  text(connected ? "online " : "offline ") |
                      color(connected ? Color::GreenLight : Color::RedLight),
              }),
              separator(),
              messages,
              separator(),
              hbox({
                  text(" > ") | bold | color(Color::CyanLight),
                  input->Render() | flex,
              }),
          }) |
          flex;

      return vbox({
                 hbox({
                     text(" znet chat ") | bold | inverted,
                     text(" " + status) | color(Color::GrayDark),
                 }),
                 hbox({
                     sidebar,
                     separator(),
                     conversation,
                 }) | flex |
                     border,
                 text(" tab: switch pane   up/down: rooms   enter: send   "
                      "esc or ctrl+c: quit") |
                     color(Color::GrayDark),
             }) |
             flex;
    });

    // ftxui::Event, spelled out: znet::Event is in scope here too.
    ui = CatchEvent(ui, [&](ftxui::Event event) {
      if (event == ftxui::Event::Escape) {
        screen.Exit();
        return true;
      }
      return false;
    });

    screen.Loop(ui);
    g_screen = nullptr;
  }

  g_session.reset();
  client.Disconnect();
  znet::Cleanup();
  return 0;
}
