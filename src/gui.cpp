// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 TheElevatedOne
// GTK3 control panel, in the spirit of vdu_controls.
#include <gtkmm.h>

#include <map>
#include <memory>

#include "session.hpp"
#include "util.hpp"

namespace dc {

namespace {

const char* kCss = R"(
.display-card { padding: 12px; }
.display-title { font-weight: bold; font-size: 110%; }
.feature-name { min-width: 7em; }
.limit-hint { font-size: 85%; }
.status-error { color: #c01c28; }
)";

// ------------------------------------------------------------- limits dialog

class LimitsDialog : public Gtk::Dialog {
 public:
  LimitsDialog(Gtk::Window& parent, Session& session, const ScreenPtr& screen)
      : Gtk::Dialog("Limits — " + screen->label(), parent, true),
        session_(session),
        screen_(screen) {
    add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    add_button("_Apply", Gtk::RESPONSE_APPLY)->get_style_context()->add_class("suggested-action");
    set_default_response(Gtk::RESPONSE_APPLY);

    auto* grid = Gtk::manage(new Gtk::Grid());
    grid->set_row_spacing(6);
    grid->set_column_spacing(12);
    grid->set_border_width(12);

    auto* intro = Gtk::manage(new Gtk::Label(
        "Restrict each control to a range. Sliders, scrolling and the waybar\n"
        "module stay inside it. With the lock on, contrast moves through its\n"
        "range in proportion to brightness."));
    intro->set_xalign(0);
    intro->get_style_context()->add_class("dim-label");
    grid->attach(*intro, 0, 0, 5, 1);

    const char* heads[] = {"Control", "Min", "Max", "Hardware"};
    for (int i = 0; i < 4; ++i) {
      auto* h = Gtk::manage(new Gtk::Label());
      h->set_markup(std::string("<b>") + heads[i] + "</b>");
      h->set_xalign(0);
      grid->attach(*h, i, 1, 1, 1);
    }

    int r = 2;
    for (const auto& f : screen->features()) {
      Row row;
      row.code = f.code;
      row.hw_max = f.max;
      row.default_min = f.default_min;
      Limits l = screen->limits(f.code);
      auto* name = Gtk::manage(new Gtk::Label(f.name));
      name->set_xalign(0);
      row.min = Gtk::manage(new Gtk::SpinButton(Gtk::Adjustment::create(l.min, 0, f.max, 1, 5)));
      row.max = Gtk::manage(new Gtk::SpinButton(Gtk::Adjustment::create(l.max, 0, f.max, 1, 5)));
      auto* hw = Gtk::manage(new Gtk::Label("0–" + std::to_string(f.max)));
      hw->get_style_context()->add_class("dim-label");
      hw->set_xalign(0);
      auto* reset = Gtk::manage(new Gtk::Button());
      reset->set_image_from_icon_name("edit-undo-symbolic");
      reset->set_tooltip_text("Reset to full range");
      reset->set_relief(Gtk::RELIEF_NONE);
      Gtk::SpinButton* mn = row.min;
      Gtk::SpinButton* mx = row.max;
      int dmin = f.default_min, hmax = f.max;
      reset->signal_clicked().connect([mn, mx, dmin, hmax] {
        mn->set_value(dmin);
        mx->set_value(hmax);
      });
      // Keep min <= max while editing.
      mn->signal_value_changed().connect([mn, mx] {
        if (mn->get_value() > mx->get_value()) mx->set_value(mn->get_value());
      });
      mx->signal_value_changed().connect([mn, mx] {
        if (mx->get_value() < mn->get_value()) mn->set_value(mx->get_value());
      });
      grid->attach(*name, 0, r, 1, 1);
      grid->attach(*row.min, 1, r, 1, 1);
      grid->attach(*row.max, 2, r, 1, 1);
      grid->attach(*hw, 3, r, 1, 1);
      grid->attach(*reset, 4, r, 1, 1);
      rows_.push_back(row);
      ++r;
    }
    get_content_area()->pack_start(*grid, true, true);
    show_all_children();
  }

  void apply() {
    for (const auto& row : rows_) {
      Limits l{row.min->get_value_as_int(), row.max->get_value_as_int()};
      if (l.min == row.default_min && l.max == row.hw_max)
        session_.config.clear_limits(screen_->id(), row.code);
      else
        session_.config.set_limits(screen_->id(), row.code, l);
    }
    session_.save_config();
  }

 private:
  struct Row {
    uint8_t code;
    int hw_max, default_min;
    Gtk::SpinButton* min;
    Gtk::SpinButton* max;
  };
  Session& session_;
  ScreenPtr screen_;
  std::vector<Row> rows_;
};

// ------------------------------------------------------------- display card

class DisplayCard : public Gtk::Frame {
 public:
  DisplayCard(Gtk::Window& parent, Session& session, ScreenPtr screen)
      : parent_(parent), session_(session), screen_(std::move(screen)) {
    get_style_context()->add_class("view");
    auto* outer = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 8));
    outer->get_style_context()->add_class("display-card");
    add(*outer);

    // Header: name, location, lock, limits.
    auto* head = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_HORIZONTAL, 6));
    auto* icon = Gtk::manage(new Gtk::Image());
    icon->set_from_icon_name(screen_->kind() == "Backlight" ? "computer-symbolic"
                                                            : "video-display-symbolic",
                             Gtk::ICON_SIZE_LARGE_TOOLBAR);
    auto* titles = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
    auto* title = Gtk::manage(new Gtk::Label(screen_->label()));
    title->set_xalign(0);
    title->get_style_context()->add_class("display-title");
    auto* sub = Gtk::manage(new Gtk::Label(screen_->kind() + " · " + screen_->location()));
    sub->set_xalign(0);
    sub->get_style_context()->add_class("dim-label");
    titles->pack_start(*title, false, false);
    titles->pack_start(*sub, false, false);
    head->pack_start(*icon, false, false);
    head->pack_start(*titles, true, true);

    lock_.set_image_from_icon_name(screen_->locked() ? "changes-prevent-symbolic"
                                                     : "changes-allow-symbolic");
    lock_.set_label("Link");
    lock_.set_always_show_image(true);
    lock_.set_active(screen_->locked());
    lock_.set_sensitive(screen_->can_lock());
    lock_.set_tooltip_text(screen_->can_lock()
                               ? "Lock contrast to brightness: moving one moves the other\n"
                                 "proportionally within their limits"
                               : "This display has no contrast control");
    lock_.signal_toggled().connect([this] {
      if (updating_) return;
      lock_.set_image_from_icon_name(lock_.get_active() ? "changes-prevent-symbolic"
                                                        : "changes-allow-symbolic");
      screen_->set_locked(lock_.get_active());
    });
    head->pack_start(lock_, false, false);

    limits_.set_image_from_icon_name("preferences-system-symbolic");
    limits_.set_tooltip_text("Set control limits");
    limits_.signal_clicked().connect([this] { open_limits(); });
    head->pack_start(limits_, false, false);
    outer->pack_start(*head, false, false);

    // Primary controls, then everything else in an expander.
    auto* main_grid = make_grid();
    auto* extra_grid = make_grid();
    int mr = 0, er = 0;
    for (const auto& f : screen_->features()) {
      bool primary = f.code == screen_->brightness_code() || f.code == screen_->contrast_code();
      if (primary) add_row(*main_grid, mr++, f);
      else add_row(*extra_grid, er++, f);
    }
    outer->pack_start(*main_grid, false, false);
    if (er > 0) {
      auto* exp = Gtk::manage(new Gtk::Expander("More controls"));
      extra_grid->set_margin_top(6);
      exp->add(*extra_grid);
      outer->pack_start(*exp, false, false);
    }

    screen_->signal_changed().connect(sigc::mem_fun(*this, &DisplayCard::on_changed));
  }

  // Re-read lock and limits from config (another process may have changed them).
  void sync_config() {
    updating_ = true;
    lock_.set_active(screen_->locked());
    lock_.set_image_from_icon_name(screen_->locked() ? "changes-prevent-symbolic"
                                                     : "changes-allow-symbolic");
    for (auto& [code, row] : rows_) {
      Limits l = screen_->limits(code);
      row.adj->set_lower(l.min);
      row.adj->set_upper(l.max);
      update_hint(row, code);
      if (const FeatureState* f = screen_->feature(code)) row.adj->set_value(f->value);
    }
    updating_ = false;
  }

 private:
  struct Row {
    Glib::RefPtr<Gtk::Adjustment> adj;
    Gtk::Label* hint = nullptr;
  };

  Gtk::Grid* make_grid() {
    auto* g = Gtk::manage(new Gtk::Grid());
    g->set_row_spacing(4);
    g->set_column_spacing(10);
    return g;
  }

  void add_row(Gtk::Grid& grid, int r, const FeatureState& f) {
    Limits l = screen_->limits(f.code);
    Row row;
    row.adj = Gtk::Adjustment::create(clampi(f.value, l.min, l.max), l.min, l.max, 1, 5);

    auto* names = Gtk::manage(new Gtk::Box(Gtk::ORIENTATION_VERTICAL, 0));
    auto* name = Gtk::manage(new Gtk::Label(f.name));
    name->set_xalign(0);
    name->get_style_context()->add_class("feature-name");
    row.hint = Gtk::manage(new Gtk::Label());
    row.hint->set_xalign(0);
    row.hint->get_style_context()->add_class("dim-label");
    row.hint->get_style_context()->add_class("limit-hint");
    names->pack_start(*name, false, false);
    names->pack_start(*row.hint, false, false);

    auto* scale = Gtk::manage(new Gtk::Scale(row.adj, Gtk::ORIENTATION_HORIZONTAL));
    scale->set_draw_value(false);
    scale->set_digits(0);
    scale->set_hexpand(true);
    scale->set_size_request(260, -1);
    auto* spin = Gtk::manage(new Gtk::SpinButton(row.adj, 1, 0));
    spin->set_width_chars(4);

    uint8_t code = f.code;
    row.adj->signal_value_changed().connect([this, code] { on_user(code); });

    grid.attach(*names, 0, r, 1, 1);
    grid.attach(*scale, 1, r, 1, 1);
    grid.attach(*spin, 2, r, 1, 1);
    rows_[code] = row;
    update_hint(rows_[code], code);
  }

  void update_hint(Row& row, uint8_t code) {
    Limits l = screen_->limits(code);
    if (screen_->has_custom_limits(code)) {
      row.hint->set_text("limit " + std::to_string(l.min) + "–" + std::to_string(l.max));
      row.hint->show();
    } else {
      row.hint->set_text("");
      row.hint->hide();
      row.hint->set_no_show_all(true);
    }
  }

  void on_user(uint8_t code) {
    if (updating_) return;
    screen_->set_value(code, static_cast<int>(rows_[code].adj->get_value() + 0.5));
  }

  void on_changed(const Changes& ch) {
    updating_ = true;
    for (auto& [code, value] : ch) {
      auto it = rows_.find(code);
      if (it != rows_.end() && static_cast<int>(it->second.adj->get_value() + 0.5) != value)
        it->second.adj->set_value(value);
    }
    updating_ = false;
  }

  void open_limits() {
    LimitsDialog dlg(parent_, session_, screen_);
    if (dlg.run() == Gtk::RESPONSE_APPLY) {
      dlg.apply();
      sync_config();
      screen_->enforce_limits();
    }
  }

  Gtk::Window& parent_;
  Session& session_;
  ScreenPtr screen_;
  Gtk::ToggleButton lock_;
  Gtk::Button limits_;
  std::map<uint8_t, Row> rows_;
  bool updating_ = false;
};

// ------------------------------------------------------------- main window

class MainWindow : public Gtk::ApplicationWindow {
 public:
  explicit MainWindow(Session& session) : session_(session) {
    set_default_size(560, -1);
    set_icon_name("video-display");

    header_.set_show_close_button(true);
    header_.set_title("DDC Controls");
    set_titlebar(header_);

    refresh_.set_image_from_icon_name("view-refresh-symbolic");
    refresh_.set_tooltip_text("Re-read values from the displays");
    refresh_.signal_clicked().connect([this] {
      for (auto& s : session_.screens) s->refresh();
    });
    rescan_.set_image_from_icon_name("system-search-symbolic");
    rescan_.set_tooltip_text("Rescan for displays");
    rescan_.signal_clicked().connect([this] { rescan(); });
    header_.pack_start(rescan_);
    header_.pack_start(refresh_);
    header_.pack_end(spinner_);

    cards_.set_border_width(12);
    cards_.set_spacing(12);
    scroll_.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll_.set_propagate_natural_height(true);
    scroll_.set_max_content_height(800);
    scroll_.add(cards_);

    status_.set_xalign(0);
    status_.set_line_wrap(true);
    status_.set_margin_start(12);
    status_.set_margin_end(12);
    status_.set_margin_bottom(8);
    status_.get_style_context()->add_class("status-error");
    status_.set_no_show_all(true);

    root_.pack_start(scroll_, true, true);
    root_.pack_start(status_, false, false);
    add(root_);

    session_.signal_screens_changed().connect([this] { populate(); });
    session_.signal_config_changed().connect([this] {
      for (auto* c : card_ptrs_) c->sync_config();
    });
    show_all_children();
    rescan();
  }

 private:
  void rescan() {
    if (session_.discovering()) return;
    spinner_.start();
    spinner_.show();
    rescan_.set_sensitive(false);
    show_message("");
    session_.discover_async([this](const std::string& log) {
      spinner_.stop();
      rescan_.set_sensitive(true);
      if (session_.screens.empty()) show_message(log);
    });
    populate();
  }

  void populate() {
    for (auto* child : cards_.get_children()) {
      cards_.remove(*child);
      delete child;
    }
    card_ptrs_.clear();
    for (auto& c : error_conns_) c.disconnect();
    error_conns_.clear();

    if (session_.discovering() && session_.screens.empty()) {
      add_placeholder("Searching for displays…");
    } else if (session_.screens.empty()) {
      add_placeholder(
          "No controllable displays found.\n\n"
          "DDC/CI needs the i2c-dev module loaded and read/write access to\n"
          "/dev/i2c-* (usually membership in the i2c group). Laptop panels are\n"
          "controlled through brightnessctl.");
    }
    for (auto& s : session_.screens) {
      auto* card = new DisplayCard(*this, session_, s);
      card_ptrs_.push_back(card);
      cards_.pack_start(*card, false, false);
      error_conns_.push_back(s->signal_error().connect(
          [this](const std::string& e) { show_message(e); }));
    }
    header_.set_subtitle(session_.discovering() ? "" :
                         std::to_string(session_.screens.size()) +
                             (session_.screens.size() == 1 ? " display" : " displays"));
    cards_.show_all();
  }

  void add_placeholder(const std::string& text) {
    auto* l = new Gtk::Label(text);
    l->set_justify(Gtk::JUSTIFY_CENTER);
    l->get_style_context()->add_class("dim-label");
    l->set_margin_top(24);
    l->set_margin_bottom(24);
    cards_.pack_start(*l, false, false);
  }

  void show_message(const std::string& text) {
    std::string t = text;
    while (!t.empty() && t.back() == '\n') t.pop_back();
    status_.set_text(t);
    status_.set_visible(!t.empty());
    msg_timer_.disconnect();
    if (!t.empty() && !session_.screens.empty())
      msg_timer_ = Glib::signal_timeout().connect_seconds([this] {
        status_.hide();
        return false;
      }, 8);
  }

  Session& session_;
  Gtk::HeaderBar header_;
  Gtk::Button refresh_, rescan_;
  Gtk::Spinner spinner_;
  Gtk::Box root_{Gtk::ORIENTATION_VERTICAL};
  Gtk::ScrolledWindow scroll_;
  Gtk::Box cards_{Gtk::ORIENTATION_VERTICAL};
  Gtk::Label status_;
  std::vector<DisplayCard*> card_ptrs_;
  std::vector<sigc::connection> error_conns_;
  sigc::connection msg_timer_;
};

}  // namespace

int run_gui(int argc, char** argv) {
  auto app = Gtk::Application::create(argc, argv, "io.github.DdcControls");
  std::unique_ptr<Session> session;
  std::unique_ptr<MainWindow> window;

  app->signal_activate().connect([&] {
    if (!window) {
      auto css = Gtk::CssProvider::create();
      css->load_from_data(kCss);
      Gtk::StyleContext::add_provider_for_screen(Gdk::Screen::get_default(), css,
                                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
      session = std::make_unique<Session>();
      session->watch();
      window = std::make_unique<MainWindow>(*session);
      app->add_window(*window);
    }
    window->present();
  });
  int rc = app->run();
  window.reset();
  session.reset();
  return rc;
}

}  // namespace dc
