/**
 * @file main.cpp
 * @brief gtk-livecss: Universal LD_PRELOAD runtime CSS hot-reloader for GTK3, GTK4, and Libadwaita.
 *
 * Architectural Design & Constraints:
 *  1. Zero Link-Time Toolkit Dependencies:
 *     Must never link directly against libgtk-3, libgtk-4, or libglib-2.0. Mapping GTK
 *     into non-GUI processes wastes memory, and loading libgtk-4 into a GTK3 or
 *     Python/PyGObject process causes GType registry collisions.
 *
 *  2. Passive Toolkit Probing (RTLD_NOLOAD):
 *     dlopen(..., RTLD_NOLOAD) checks whether the host process has already mapped
 *     libgtk-4.so.1 or libgtk-3.so.0 without loading libraries from disk or running
 *     ELF constructors.
 *
 *  3. Lock-Free Initialization & Main-Thread Affinity:
 *     GLib/GTK objects (GtkCssProvider, GFileMonitor) are not thread-safe and are
 *     created/mutated strictly on GLib's default GMainContext thread. Initialization
 *     uses std::atomic flags rather than std::call_once + dlopen() to avoid lock-order
 *     inversion deadlocks with glibc's internal _rtld_global lock.
 *
 *  4. Process-Lifetime Ownership:
 *     The singleton ReloadContext is intentionally retained until process exit rather than
 *     destroyed in an __attribute__((destructor)), as libgtk/libgobject may already be
 *     torn down during exit() finalization.
 *
 * @license MIT
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <atomic>
#include <cstdio>
#include <dlfcn.h>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace {
    namespace fs = std::filesystem;

    enum class GtkVersion {
        None,
        Gtk3,
        Gtk4
    };

    // -------------------------------------------------------------------------
    // GLib / GIO ABI Function Signatures
    // -------------------------------------------------------------------------
    using g_get_user_config_dir_t    = const char* (*)();
    using g_file_new_for_path_t      = void* (*)(const char*);
    using g_file_monitor_directory_t = void* (*)(void*, int, void*, void**);
    using g_signal_connect_data_t    = unsigned long (*)(void*, const char*, void*, void*, void*, int);
    using g_timeout_add_t            = unsigned int (*)(unsigned int, int (*)(void*), void*);
    using g_source_remove_t          = int (*)(unsigned int);
    using g_file_get_basename_t      = char* (*)(void*);
    using g_free_t                   = void (*)(void*);
    using g_object_unref_t           = void (*)(void*);

    // -------------------------------------------------------------------------
    // GTK3 & GTK4 ABI Function Signatures
    // Note: gtk_css_provider_load_from_file() differs in arity between GTK3
    // (3 args, trailing GError**) and GTK4 (2 args, void return).
    // -------------------------------------------------------------------------
    using gtk_css_provider_new_t                       = void* (*)();
    using gdk_display_get_default_t                    = void* (*)();
    using gdk_screen_get_default_t                     = void* (*)();
    using gtk_style_context_add_provider_for_display_t = void (*)(void*, void*, unsigned int);
    using gtk_style_context_add_provider_for_screen_t  = void (*)(void*, void*, unsigned int);
    using gtk_css_provider_load_from_file_gtk4_t       = void (*)(void*, void*);
    using gtk_css_provider_load_from_file_gtk3_t       = int (*)(void*, void*, void**);

    /**
     * @brief Per-process state bound to the active GdkDisplay (GTK4) or GdkScreen (GTK3).
     */
    struct ReloadContext {
        GtkVersion version = GtkVersion::None;
        void* gtk_handle = nullptr;
        void* provider = nullptr;
        void* monitor = nullptr;
        std::string css_path;
        unsigned int debounce_source_id = 0;
    };

    std::unique_ptr<ReloadContext> g_ctx = nullptr;
    std::atomic<bool> g_poller_active{false};
    std::atomic<bool> g_attached{false};

    /**
     * @brief Resolves a symbol from an explicit library handle first, falling back to RTLD_DEFAULT.
     *
     * Explicit handle lookup is required for Python/PyGObject and GJS, which load
     * GI typelib libraries with RTLD_LOCAL (invisible to RTLD_DEFAULT).
     */
    template <typename T>
    T resolve_sym(void* handle, const char* symbol) {
        if (handle) {
            if (void* ptr = dlsym(handle, symbol)) {
                return reinterpret_cast<T>(ptr);
            }
        }
        return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, symbol));
    }

    /**
     * @brief Suppresses GtkCssProvider::parsing-error warnings on stderr if a theme
     *        generator updates files mid-flight or uses toolkit-specific variables.
     */
    void on_css_parsing_error(void*, void*, void*, void*) {}

    /**
     * @brief Reloads gtk.css (and any @import'd stylesheets such as colors.css) into the provider.
     */
    void reload_css_now(const ReloadContext* ctx) {
        if (!ctx || !ctx->provider || !ctx->gtk_handle) return;

        std::error_code ec;
        if (const fs::path path(ctx->css_path); !fs::exists(path, ec) || fs::file_size(path, ec) == 0) return;

        const auto file_new = resolve_sym<g_file_new_for_path_t>(ctx->gtk_handle, "g_file_new_for_path");
        const auto unref    = resolve_sym<g_object_unref_t>(ctx->gtk_handle, "g_object_unref");
        if (!file_new || !unref) return;

        void* gfile = file_new(ctx->css_path.c_str());
        if (!gfile) return;

        if (ctx->version == GtkVersion::Gtk4) {
            auto load_gtk4 = resolve_sym<gtk_css_provider_load_from_file_gtk4_t>(
                ctx->gtk_handle, "gtk_css_provider_load_from_file");
            if (load_gtk4) load_gtk4(ctx->provider, gfile);
        } else if (ctx->version == GtkVersion::Gtk3) {
            auto load_gtk3 = resolve_sym<gtk_css_provider_load_from_file_gtk3_t>(
                ctx->gtk_handle, "gtk_css_provider_load_from_file");
            if (load_gtk3) load_gtk3(ctx->provider, gfile, nullptr);
        }

        unref(gfile);
    }

    /**
     * @brief GLib timeout callback executed once stylesheet writes have settled.
     */
    int debounced_reload_cb(void* user_data) {
        auto* ctx = static_cast<ReloadContext*>(user_data);
        if (!ctx) return 0; // G_SOURCE_REMOVE
        ctx->debounce_source_id = 0;
        reload_css_now(ctx);
        return 0; // G_SOURCE_REMOVE
    }

    /**
     * @brief GFileMonitor::changed callback watching ~/.config/gtk-{3,4}.0/.
     *
     * Theme generators (e.g., matugen) typically update colors.css via atomic temp-file
     * rename while gtk.css imports it via `@import 'colors.css';`. Matching any non-hidden
     * `*.css` file and debouncing by 50 ms avoids reading truncated files mid-rename.
     */
    void on_directory_changed(void*, void* file, void*, int, void* user_data) {
        auto* ctx = static_cast<ReloadContext*>(user_data);
        if (!ctx || !file) return;

        auto get_basename = resolve_sym<g_file_get_basename_t>(ctx->gtk_handle, "g_file_get_basename");
        auto g_free_fn    = resolve_sym<g_free_t>(ctx->gtk_handle, "g_free");
        if (!get_basename) return;

        char* raw_name = get_basename(file);
        if (!raw_name) return;

        std::string_view name(raw_name);
        const bool is_css = name.size() > 4 && name.ends_with(".css") && !name.starts_with('.');
        if (g_free_fn) g_free_fn(raw_name);

        if (!is_css) return;

        auto timeout_add   = resolve_sym<g_timeout_add_t>(ctx->gtk_handle, "g_timeout_add");
        auto source_remove = resolve_sym<g_source_remove_t>(ctx->gtk_handle, "g_source_remove");
        if (!timeout_add) return;

        if (ctx->debounce_source_id != 0 && source_remove) {
            source_remove(ctx->debounce_source_id);
            ctx->debounce_source_id = 0;
        }
        ctx->debounce_source_id = timeout_add(50, debounced_reload_cb, ctx);
    }

    /**
     * @brief Registers a high-priority GtkCssProvider (priority 801 = USER + 1) and attaches
     *        a GFileMonitor to the user's GTK configuration directory.
     */
    bool attach_gtk_monitor(GtkVersion ver, void* gtk_handle, void* target_surface) {
        auto get_config_dir = resolve_sym<g_get_user_config_dir_t>(gtk_handle, "g_get_user_config_dir");
        auto provider_new   = resolve_sym<gtk_css_provider_new_t>(gtk_handle, "gtk_css_provider_new");
        auto file_new       = resolve_sym<g_file_new_for_path_t>(gtk_handle, "g_file_new_for_path");
        auto monitor_dir    = resolve_sym<g_file_monitor_directory_t>(gtk_handle, "g_file_monitor_directory");
        auto signal_connect = resolve_sym<g_signal_connect_data_t>(gtk_handle, "g_signal_connect_data");
        auto unref          = resolve_sym<g_object_unref_t>(gtk_handle, "g_object_unref");

        if (!get_config_dir || !provider_new || !file_new || !monitor_dir || !signal_connect || !unref) {
            return false;
        }

        const char* base_dir = get_config_dir();
        if (!base_dir) return false;

        fs::path config_dir = fs::path(base_dir) / (ver == GtkVersion::Gtk4 ? "gtk-4.0" : "gtk-3.0");
        fs::path css_path   = config_dir / "gtk.css";

        std::error_code ec;
        fs::create_directories(config_dir, ec);

        void* provider = provider_new();
        if (!provider) return false;

        signal_connect(provider, "parsing-error",
                       reinterpret_cast<void*>(on_css_parsing_error), nullptr, nullptr, 0);

        auto ctx = std::make_unique<ReloadContext>();
        ctx->version    = ver;
        ctx->gtk_handle = gtk_handle;
        ctx->provider   = provider;
        ctx->css_path   = css_path.string();

        reload_css_now(ctx.get());

        // Priority 801 (GTK_STYLE_PROVIDER_PRIORITY_USER + 1) overrides static startup CSS
        if (ver == GtkVersion::Gtk4) {
            auto add_display_provider = resolve_sym<gtk_style_context_add_provider_for_display_t>(
                gtk_handle, "gtk_style_context_add_provider_for_display");
            if (add_display_provider) add_display_provider(target_surface, provider, 801);
        } else {
            auto add_screen_provider = resolve_sym<gtk_style_context_add_provider_for_screen_t>(
                gtk_handle, "gtk_style_context_add_provider_for_screen");
            if (add_screen_provider) add_screen_provider(target_surface, provider, 801);
        }

        void* dir_gfile = file_new(config_dir.c_str());
        if (!dir_gfile) return false;

        void* monitor = monitor_dir(dir_gfile, 0, nullptr, nullptr);
        unref(dir_gfile);
        if (!monitor) return false;

        ctx->monitor = monitor;
        signal_connect(monitor, "changed",
                       reinterpret_cast<void*>(on_directory_changed), ctx.get(), nullptr, 0);

        g_ctx = std::move(ctx);
        g_attached.store(true, std::memory_order_release);
        return true;
    }

    /**
     * @brief Polls at 100 ms intervals on the GLib main loop until GTK initializes a default
     *        GdkDisplay (GTK4) or GdkScreen (GTK3). Stops cleanly after 10 seconds for
     *        headless GLib daemons.
     */
    int poll_for_gtk_display(void*) {
        if (g_attached.load(std::memory_order_acquire)) return 0;

        static int attempts = 0;
        if (++attempts > 100) {
            g_poller_active.store(false, std::memory_order_release);
            return 0; // G_SOURCE_REMOVE
        }

        if (void* h4 = dlopen("libgtk-4.so.1", RTLD_LAZY | RTLD_NOLOAD)) {
            if (auto get_display = resolve_sym<gdk_display_get_default_t>(h4, "gdk_display_get_default")) {
                if (void* display = get_display()) {
                    if (attach_gtk_monitor(GtkVersion::Gtk4, h4, display)) return 0;
                }
            }
        }

        if (void* h3 = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_NOLOAD)) {
            if (auto get_screen = resolve_sym<gdk_screen_get_default_t>(h3, "gdk_screen_get_default")) {
                if (void* screen = get_screen()) {
                    if (attach_gtk_monitor(GtkVersion::Gtk3, h3, screen)) return 0;
                }
            }
        }

        return 1; // G_SOURCE_CONTINUE
    }

    /**
     * @brief Schedules the display poller on GLib's default main context if libglib-2.0 is loaded.
     */
    void schedule_poller() {
        if (g_attached.load(std::memory_order_acquire) || g_poller_active.load(std::memory_order_acquire)) {
            return;
        }

        void* glib = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
        if (!glib) return;

        if (auto timeout_add = resolve_sym<g_timeout_add_t>(glib, "g_timeout_add"); timeout_add && !g_poller_active.exchange(true, std::memory_order_acq_rel)) {
            timeout_add(100, poll_for_gtk_display, nullptr);
        }
    }
} // namespace

// -----------------------------------------------------------------------------
// Dynamic Linker Interposition Entry Points
// Marked `visibility("default")` and `used` so -fvisibility=hidden and LTO
// preserve all PLT/GOT hooks in the exported .dynsym table.
// -----------------------------------------------------------------------------
#define EXPORT_HOOK __attribute__((visibility("default"), used))

extern "C" {
    __attribute__((constructor))
    static void preload_init() {
        schedule_poller();
    }

    EXPORT_HOOK void gtk_init(int* argc, char*** argv) {
        using fn_t = void (*)(int*, char***);
        static auto real_fn = reinterpret_cast<fn_t>(dlsym(RTLD_NEXT, "gtk_init"));
        if (real_fn) real_fn(argc, argv);
        schedule_poller();
    }

    EXPORT_HOOK int gtk_init_check(int* argc, char*** argv) {
        using fn_t = int (*)(int*, char***);
        static auto real_fn = reinterpret_cast<fn_t>(dlsym(RTLD_NEXT, "gtk_init_check"));
        int res = real_fn ? real_fn(argc, argv) : 0;
        schedule_poller();
        return res;
    }

    EXPORT_HOOK int g_application_run(void* application, int argc, char** argv) {
        using fn_t = int (*)(void*, int, char**);
        static auto real_fn = reinterpret_cast<fn_t>(dlsym(RTLD_NEXT, "g_application_run"));
        schedule_poller();
        return real_fn ? real_fn(application, argc, argv) : 0;
    }

    EXPORT_HOOK void g_main_loop_run(void* loop) {
        using fn_t = void (*)(void*);
        static auto real_fn = reinterpret_cast<fn_t>(dlsym(RTLD_NEXT, "g_main_loop_run"));
        schedule_poller();
        if (real_fn) real_fn(loop);
    }

    EXPORT_HOOK int g_main_context_iteration(void* context, int may_block) {
        using fn_t = int (*)(void*, int);
        static auto real_fn = reinterpret_cast<fn_t>(dlsym(RTLD_NEXT, "g_main_context_iteration"));
        if (!g_attached.load(std::memory_order_acquire)) {
            schedule_poller();
        }
        return real_fn ? real_fn(context, may_block) : 0;
    }
}