#include "gtk_callbacks.h"
#include <iostream>
#include <future>
#include "MainPickerWindow.h"
#include "gtk_input_appid_game_row.h"
#include "../common/PerfMon.h"
#include "../MySteam.h"
#include "../globals.h"

// See comments in the header file

extern "C"
{

    void
    populate_achievements() {
        // Get_achievements from game server
        std::vector<Achievement_t> achievements = g_steam->get_achievements();

        g_main_gui->reset_achievements_list();

        // Assigning special to achievements
        long next_most_achieved_index = -1;
        float next_most_achieved_rate = 0;
        bool ach_is_special;
        Achievement_t tmp;
        for(size_t i = 0; i < achievements.size(); i++) {
            tmp = achievements[i];
            ach_is_special = false;
            if ( !tmp.achieved && tmp.global_achieved_rate > next_most_achieved_rate )
            {
                next_most_achieved_rate = tmp.global_achieved_rate;
                next_most_achieved_index = i;
            }

            if ( tmp.global_achieved_rate <= 5.f )
            {
                ach_is_special = true;
                achievements[i].special = ACHIEVEMENT_RARE;
            }
            
            if ( !ach_is_special )
            {
                achievements[i].special = ACHIEVEMENT_NORMAL;
            }
        }

        if (next_most_achieved_index != -1)
        {
            achievements[next_most_achieved_index].special |= ACHIEVEMENT_NEXT_MOST_ACHIEVED;
        }
        
        for(Achievement_t achievement : achievements) {
            g_main_gui->add_to_achievement_list(achievement);
        }

        g_main_gui->confirm_achievement_list();
    }
    // => populate_achievements

    void 
    on_close_button_clicked() {
        g_main_gui->stop();
        
        delete g_main_gui;
        g_main_gui = nullptr;

        g_steam->quit_game();
    }
    // => on_close_button_clicked

    void
    on_store_button_clicked() {
        std::cerr << "Saving stats and achievements." << std::endl;

        g_steam->commit_changes();

        populate_achievements();
    }
    // => on_store_button_clicked

    /*
     * The actual loading function
     * 
     * We CANNOT update the GUI from any thread but the main thread because
     * it is explicitly deprecated in gtk...
     * (e.g. https://developer.gnome.org/gdk3/stable/gdk3-Threads.html#gdk-threads-init)
     * See the gtk_callbacks.h for the FSM rationale.
     * 
     * For anything that isn't the GUI, we can fire off worker threads, and doing
     * so is simpler than splitting out the worker threads into the main GUI loop.
     * Additionally, the worker threads depend on calling functions which may
     * take a while to return. So, splitting them out into a thread won't expose the
     * main loop to these latencies and potentially make it laggy.
     */
    static gboolean
    load_items_idle (gpointer data_)
    {
        IdleData *data = (IdleData *)data_;

        if (data->state == STATE_STARTED) {
            g_main_gui->reset_game_list();
            g_perfmon->log("Starting library parsing.");
            g_main_gui->owned_apps_future = std::async(std::launch::async, []{g_steam->refresh_owned_apps();});
            data->state = STATE_WAITING_FOR_OWNED_APPS;
            return G_SOURCE_CONTINUE;
        }

        if (data->state == STATE_WAITING_FOR_OWNED_APPS) {
            if (g_main_gui->owned_apps_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                g_perfmon->log("Done retrieving and filtering owned apps");
                data->state = STATE_LOADING_GUI;
            }
            return G_SOURCE_CONTINUE;
        }

        if (data->state == STATE_LOADING_GUI) {
            if (data->current_item == g_steam->get_subscribed_apps().size()) {
                g_perfmon->log("Done adding apps to GUI");
                g_main_gui->confirm_game_list();
                data->state = STATE_DOWNLOADING_ICONS;
                data->current_item = 0;
                return G_SOURCE_CONTINUE;
            }

            Game_t app = g_steam->get_subscribed_apps()[data->current_item];
            g_main_gui->add_to_game_list(app);
            data->current_item++;
            return G_SOURCE_CONTINUE;
        }

        if (data->state == STATE_DOWNLOADING_ICONS) {
            // This must occur after the main gui game_list is
            // complete, otherwise we might have concurrent
            // access and modification of the GUI's game_list

            bool done_starting_downloads = (data->current_item == g_steam->get_subscribed_apps().size());

            // Make sure we're done starting all downloads and finshed with outstanding downloads
            if (done_starting_downloads && (g_main_gui->icon_download_futures.size() == 0)) {
                g_perfmon->log("Done downloading icons");
                data->state = STATE_FINISHED;
                return G_SOURCE_REMOVE;
            }

            // We could have the threads IPC the data back to the main thread
            // but that's a bit heavyweight for what we need here:
            // the main thread just needs to know the app_id that completed, and
            // it can figure out the rest.

            // Implement a poor man's thread pool for now - only fire off
            // a few threads at a time (though threads are not recycled).
            // We would use a semaphore here for outstanding_icon_downloads,
            // but the GTK main loop is forcibly single-threaded
            // (the whole reason we need to do these shenanigans anyway),
            // so only 1 thread will ever be here at a time anyway.
            if ( !done_starting_downloads && (g_main_gui->outstanding_icon_downloads < MAX_OUTSTANDING_ICON_DOWNLOADS))  {
                // Fire off a new download thread
                Game_t app = g_steam->get_subscribed_apps()[data->current_item];
                g_main_gui->icon_download_futures.insert(std::make_pair(app.app_id, std::async(std::launch::async, g_steam->refresh_icon, app.app_id)));
                g_main_gui->outstanding_icon_downloads++;
                data->current_item++;

                // continue on to service a thread if it's finished
            }

            // Try to find a thread that is finished. Only process at most 1 per GTK main loop.
            // The max time this takes to traverse is controlled by the size of the
            // icon_download_futures size, which is controlled by MAX_ICON_DOWNLOADS.
            // Increasing this could lead to GUI stutter if it needs to traverse a large map,
            // although the map has logarithmic traversal and update complexity.
            for (auto const& [app_id, this_future] : g_main_gui->icon_download_futures) {
                if (this_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    // TODO: remove the app if it has a bad icon? (because then it's mostly likely not a game)
                    g_main_gui->refresh_app_icon(app_id);
                    g_main_gui->icon_download_futures.erase(app_id);
                    g_main_gui->outstanding_icon_downloads--;
                    // let's only process one at a time
                    return G_SOURCE_CONTINUE;
                }
            }

            return G_SOURCE_CONTINUE;
        }

        // Should never reach here
        return G_SOURCE_CONTINUE;
    }
    // => load_items_idle

    /* the finish function */
    static void
    finish_load_items (gpointer data_)
    {
        IdleData *data = (IdleData *)data_;
        g_perfmon->log("Library parsed.");
        g_free(data);
        g_main_gui->m_game_refresh_lock.unlock();
        g_main_gui->show_no_games_found_placeholder();
    }
    // => finish_load_items

    void
    on_about_button_clicked() {
        g_main_gui->show_about_dialog();
    }

    void
    on_about_dialog_close_button_clicked() {
        g_main_gui->hide_about_dialog();
    }

    void 
    on_refresh_games_button_clicked() {
        if (g_main_gui->m_game_refresh_lock.try_lock()) {
            IdleData *data;

            data = g_new(IdleData, 1);
            data->current_item = 0;
            data->state = STATE_STARTED;
            g_main_gui->outstanding_icon_downloads = 0;
            g_main_gui->show_fetch_games_placeholder();

            // Use low priority so we don't block showing the main window
            // This allows the main window to show up immediately
            g_idle_add_full (G_PRIORITY_LOW,
                            load_items_idle,
                            data,
                            finish_load_items);
        } else {
            std::cerr << "Not refreshing games because a refresh is already in progress" << std::endl;
        }
    }
    // => on_refresh_games_button_clicked

    void
    on_refresh_achievements_button_clicked() {
        g_steam->clear_changes();
        populate_achievements();
    }
    // => on_refresh_achievements_button_clicked

    void
    on_unlock_all_achievements_button_clicked() {
        g_main_gui->unlock_all_achievements();
    }
    // => on_unlock_all_achievements_button_clicked

    void
    on_lock_all_achievements_button_clicked() {
        g_main_gui->lock_all_achievements();
    }
    // => on_lock_all_achievements_button_clicked
  
    void
    on_invert_all_achievements_button_clicked() {
        g_main_gui->invert_all_achievements();
    }
    // => on_invert_all_achievements_button_clicked

    void 
    on_main_window_show() {
        on_refresh_games_button_clicked();
    }
    // => on_main_window_show

    void
    on_game_search_changed(GtkWidget* search_widget) {
        const char* filter_text = gtk_entry_get_text( GTK_ENTRY(search_widget) );
        g_main_gui->filter_games(filter_text);
    }
    // => on_game_search_changed

    void
    on_achievement_search_changed(GtkWidget* search_widget) {
        const char* filter_text = gtk_entry_get_text( GTK_ENTRY(search_widget) );
        g_main_gui->filter_achievements(filter_text);
    }
    // => on_achievement_search_changed

    void 
    on_game_row_activated(GtkListBox *box, GtkListBoxRow *row) {

        AppId_t appId = g_main_gui->get_corresponding_appid_for_row(row);
        
        if ( appId == 0 ) {
            appId = gtk_input_appid_game_row_get_appid(row);
        }
        
        if ( appId == 0 ) {
            std::cerr << "An error occurred figuring out which app to launch.. You can report this to the developer." << std::endl;
            return;
        }

        // Currently this doesn't actually show the fetch_achievements_placeholder
        // because the thread gets blocked behind populate_achievements and gtk_main
        // never gets a chance to run and refresh the window before it's replaced
        // with achievement rows.
        // So TODO: fire this populate_achievements in a different thread to not
        //          block main thread?
        g_main_gui->show_fetch_achievements_placeholder();
        g_main_gui->switch_to_achievement_page();
        g_steam->launch_game(appId);
        populate_achievements();
        g_main_gui->show_no_achievements_found_placeholder();

    }
    // => on_game_row_activated

    void
    on_back_button_clicked() {
        g_steam->quit_game();
        g_main_gui->switch_to_games_page();
    }
    // => on_back_button_clicked
}