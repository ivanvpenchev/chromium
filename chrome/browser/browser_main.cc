// Copyright 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <windows.h>
#include <shellapi.h>

#include <algorithm>

#include "base/command_line.h"
#include "base/file_util.h"
#include "base/gfx/vector_canvas.h"
#include "base/histogram.h"
#include "base/path_service.h"
#include "base/registry.h"
#include "base/string_util.h"
#include "base/tracked_objects.h"
#include "chrome/app/google_update_settings.h"
#include "chrome/app/result_codes.h"
#include "chrome/browser/automation/automation_provider.h"
#include "chrome/browser/browser.h"
#include "chrome/browser/browser_init.h"
#include "chrome/browser/browser_list.h"
#include "chrome/browser/browser_prefs.h"
#include "chrome/browser/browser_process_impl.h"
#include "chrome/browser/browser_shutdown.h"
#include "chrome/browser/cert_store.h"
#include "chrome/browser/dom_ui/chrome_url_data_manager.h"
#include "chrome/browser/first_run.h"
#include "chrome/browser/jankometer.h"
#include "chrome/browser/metrics_service.h"
#include "chrome/browser/net/dns_global.h"
#include "chrome/browser/plugin_service.h"
#include "chrome/browser/printing/print_job_manager.h"
#include "chrome/browser/rlz/rlz.h"
#include "chrome/browser/shell_integration.h"
#include "chrome/browser/url_fixer_upper.h"
#include "chrome/browser/user_data_dir_dialog.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/env_util.h"
#include "chrome/common/env_vars.h"
#include "chrome/common/jstemplate_builder.h"
#include "chrome/common/l10n_util.h"
#include "chrome/common/resource_bundle.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/pref_service.h"
#include "chrome/common/win_util.h"
#include "chrome/views/accelerator_handler.h"
#include "net/base/net_module.h"
#include "net/base/net_resources.h"
#include "net/base/net_util.h"
#include "net/base/winsock_init.h"
#include "net/http/http_network_layer.h"

#include "generated_resources.h"

namespace {

// This function provides some ways to test crash and assertion handling
// behavior of the program.
void HandleErrorTestParameters(const CommandLine& command_line) {
  // This parameter causes an assertion.
  if (command_line.HasSwitch(switches::kBrowserAssertTest)) {
    DCHECK(false);
  }

  // This parameter causes a null pointer crash (crash reporter trigger).
  if (command_line.HasSwitch(switches::kBrowserCrashTest)) {
    int* bad_pointer = NULL;
    *bad_pointer = 0;
  }
}

// This is called indirectly by the network layer to access resources.
std::string NetResourceProvider(int key) {
  const std::string& data_blob =
      ResourceBundle::GetSharedInstance().GetDataResource(key);
  if (IDR_DIR_HEADER_HTML == key) {
    DictionaryValue value;
    value.SetString(L"header",
                    l10n_util::GetString(IDS_DIRECTORY_LISTING_HEADER));
    value.SetString(L"parentDirText",
                    l10n_util::GetString(IDS_DIRECTORY_LISTING_PARENT));
    value.SetString(L"headerName",
                    l10n_util::GetString(IDS_DIRECTORY_LISTING_NAME));
    value.SetString(L"headerSize",
                    l10n_util::GetString(IDS_DIRECTORY_LISTING_SIZE));
    value.SetString(L"headerDateModified",
                    l10n_util::GetString(IDS_DIRECTORY_LISTING_DATE_MODIFIED));
    return jstemplate_builder::GetTemplateHtml(data_blob, &value, "t");
  }

  return data_blob;
}

// Displays a warning message if the user is running chrome on windows 2000.
// Returns true if the OS is win2000, false otherwise.
bool CheckForWin2000() {
  if (win_util::GetWinVersion() == win_util::WINVERSION_2000) {
    const std::wstring text = l10n_util::GetString(IDS_UNSUPPORTED_OS_WIN2000);
    const std::wstring caption = l10n_util::GetString(IDS_PRODUCT_NAME);
    win_util::MessageBox(NULL, text, caption,
                         MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return true;
  }
  return false;
}

bool AskForUninstallConfirmation() {
  const std::wstring text = l10n_util::GetString(IDS_UNINSTALL_VERIFY);
  const std::wstring caption = l10n_util::GetString(IDS_PRODUCT_NAME);
  const UINT flags = MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST;
  return (IDOK == win_util::MessageBox(NULL, text, caption, flags));
}

// Prepares the localized strings that are going to be displayed to
// the user if the browser process dies. These strings are stored in the
// environment block so they are accessible in the early stages of the
// chrome executable's lifetime.
void PrepareRestartOnCrashEnviroment(const CommandLine &parsed_command_line) {
  // Clear this var so child processes don't show the dialog by default.
  ::SetEnvironmentVariableW(env_vars::kShowRestart, NULL);

  // For non-interactive tests we don't restart on crash.
  if (::GetEnvironmentVariableW(env_vars::kHeadless, NULL, 0))
    return;

  // If the known command-line test options are used we don't create the
  // environment block which means we don't get the restart dialog.
  if (parsed_command_line.HasSwitch(switches::kBrowserCrashTest) ||
      parsed_command_line.HasSwitch(switches::kBrowserAssertTest) ||
      parsed_command_line.HasSwitch(switches::kNoErrorDialogs))
    return;

  // The encoding we use for the info is "title|context|direction" where
  // direction is either env_vars::kRtlLocale or env_vars::kLtrLocale depending
  // on the current locale.
  std::wstring dlg_strings;
  dlg_strings.append(l10n_util::GetString(IDS_CRASH_RECOVERY_TITLE));
  dlg_strings.append(L"|");
  dlg_strings.append(l10n_util::GetString(IDS_CRASH_RECOVERY_CONTENT));
  dlg_strings.append(L"|");
  if (l10n_util::GetTextDirection() == l10n_util::RIGHT_TO_LEFT)
    dlg_strings.append(env_vars::kRtlLocale);
  else
    dlg_strings.append(env_vars::kLtrLocale);

  ::SetEnvironmentVariableW(env_vars::kRestartInfo, dlg_strings.c_str());
}

int DoUninstallTasks() {
  if (!AskForUninstallConfirmation())
    return ResultCodes::UNINSTALL_USER_CANCEL;
  // The following actions are just best effort.
  LOG(INFO) << "Executing uninstall actions";
  ResultCodes::ExitCode ret = ResultCodes::NORMAL_EXIT;
  if (!FirstRun::RemoveSentinel())
    ret = ResultCodes::UNINSTALL_DELETE_FILE_ERROR;
  if (!FirstRun::RemoveChromeDesktopShortcut())
    ret = ResultCodes::UNINSTALL_DELETE_FILE_ERROR;
  if (!FirstRun::RemoveChromeQuickLaunchShortcut())
    ret = ResultCodes::UNINSTALL_DELETE_FILE_ERROR;
  return ret;
}

// This method handles the --hide-icons and --show-icons command line options
// for chrome that get triggered by Windows from registry entries
// HideIconsCommand & ShowIconsCommand. Chrome doesn't support hide icons
// functionality so we just ask the users if they want to uninstall Chrome.
int HandleIconsCommands(const CommandLine &parsed_command_line) {
  if (parsed_command_line.HasSwitch(switches::kHideIcons)) {
    OSVERSIONINFO version = {0};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!GetVersionEx(&version))
      return ResultCodes::UNSUPPORTED_PARAM;

    std::wstring cp_applet;
    if (version.dwMajorVersion >= 6) {
      cp_applet.assign(L"Programs and Features");  // Windows Vista and later.
    } else if (version.dwMajorVersion == 5 && version.dwMinorVersion >= 1) {
      cp_applet.assign(L"Add/Remove Programs");  // Windows XP.
    } else {
      return ResultCodes::UNSUPPORTED_PARAM;  // Not supported on Win2K?
    }

    const std::wstring msg = l10n_util::GetStringF(IDS_HIDE_ICONS_NOT_SUPPORTED,
                                                   cp_applet);
    const std::wstring caption = l10n_util::GetString(IDS_PRODUCT_NAME);
    const UINT flags = MB_OKCANCEL | MB_ICONWARNING | MB_TOPMOST;
    if (IDOK == win_util::MessageBox(NULL, msg, caption, flags))
      ShellExecute(NULL, NULL, L"appwiz.cpl", NULL, NULL, SW_SHOWNORMAL);
    return ResultCodes::NORMAL_EXIT;  // Exit as we are not launching browser.
  }
  // We don't hide icons so we shouldn't do anything special to show them
  return ResultCodes::UNSUPPORTED_PARAM;
}

bool DoUpgradeTasks(const CommandLine& command_line) {
  if (!Upgrade::SwapNewChromeExeIfPresent())
    return false;
  // At this point the chrome.exe has been swapped with the new one.
  if (!Upgrade::RelaunchChromeBrowser(command_line)) {
    // The re-launch fails. Feel free to panic now.
    NOTREACHED();
  }
  return true;
}

}  // namespace

// Main routine for running as the Browser process.
int BrowserMain(CommandLine &parsed_command_line, int show_command,
                sandbox::BrokerServices* broker_services) {
  // WARNING: If we get a WM_ENDSESSION objects created on the stack here
  // are NOT deleted. If you need something to run during WM_ENDSESSION add it
  // to browser_shutdown::Shutdown or BrowserProcess::EndSession.

  // TODO(beng, brettw): someday, break this out into sub functions with well
  //                     defined roles (e.g. pre/post-profile startup, etc).

  const char* main_thread_name = "Chrome_BrowserMain";
  Thread::SetThreadName(main_thread_name, GetCurrentThreadId());
  MessageLoop::current()->SetThreadName(main_thread_name);

  // Make the selection of network stacks early on before any consumers try to
  // issue HTTP requests.
  if (parsed_command_line.HasSwitch(switches::kUseNewHttp))
    net::HttpNetworkLayer::UseWinHttp(false);

  std::wstring exe;
  PathService::Get(base::FILE_EXE, &exe);
  std::replace(exe.begin(), exe.end(), '\\', '!');
  std::transform(exe.begin(), exe.end(), exe.begin(), tolower);
  HANDLE handle = CreateEvent(NULL, TRUE, TRUE, exe.c_str());
  bool already_running = false;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    already_running = true;
    CloseHandle(handle);
  }

  std::wstring user_data_dir;
  PathService::Get(chrome::DIR_USER_DATA, &user_data_dir);
  BrowserInit::MessageWindow message_window(user_data_dir);

  scoped_ptr<BrowserProcess> browser_process;
  if (parsed_command_line.HasSwitch(switches::kImport)) {
    // We use different BrowserProcess when importing so no GoogleURLTracker is
    // instantiated (as it makes a URLRequest and we don't have an IO thread,
    // see bug #1292702).
    browser_process.reset(new FirstRunBrowserProcess(parsed_command_line));
  } else {
    browser_process.reset(new BrowserProcessImpl(parsed_command_line));
  }

  // BrowserProcessImpl's constructor should set g_browser_process.
  DCHECK(g_browser_process);

  // Load local state.  This includes the application locale so we know which
  // locale dll to load.
  PrefService* local_state = browser_process->local_state();
  DCHECK(local_state);

  bool is_first_run = FirstRun::IsChromeFirstRun() ||
      parsed_command_line.HasSwitch(switches::kFirstRun);

  // Initialize ResourceBundle which handles files loaded from external
  // sources. This has to be done before uninstall code path and before prefs
  // are registered.
  local_state->RegisterStringPref(prefs::kApplicationLocale, L"");
  local_state->RegisterBooleanPref(prefs::kMetricsReportingEnabled, false);

  // During first run we read the google update registry key to find what
  // language the user selected when downloading the installer. This
  // becomes our default language in the prefs.
  if (is_first_run) {
    std::wstring install_lang;
    if (GoogleUpdateSettings::GetLanguage(&install_lang))
      local_state->SetString(prefs::kApplicationLocale, install_lang);
    if (GoogleUpdateSettings::GetCollectStatsConsent())
      local_state->SetBoolean(prefs::kMetricsReportingEnabled, true);
  }

  ResourceBundle::InitSharedInstance(
      local_state->GetString(prefs::kApplicationLocale));
  // We only load the theme dll in the browser process.
  ResourceBundle::GetSharedInstance().LoadThemeResources();

  if (!parsed_command_line.HasSwitch(switches::kNoErrorDialogs)) {
    // Display a warning if the user is running windows 2000.
    CheckForWin2000();
  }

  // Initialize histogram statistics gathering system.
  StatisticsRecorder statistics;

  // Strart tracking the creation and deletion of Task instances
  bool tracking_objects = false;
#ifdef TRACK_ALL_TASK_OBJECTS
  tracking_objects = tracked_objects::ThreadData::StartTracking(true);
#endif

  // Try to create/load the profile.
  ProfileManager* profile_manager = browser_process->profile_manager();
  Profile* profile = profile_manager->GetDefaultProfile(user_data_dir);
  if (!profile) {
    user_data_dir = UserDataDirDialog::RunUserDataDirDialog(user_data_dir);
    // Flush the message loop which lets the UserDataDirDialog close.
    MessageLoop::current()->Run();

    ResourceBundle::CleanupSharedInstance();

    if (!user_data_dir.empty()) {
      // Because of the way CommandLine parses, it's sufficient to append a new
      // --user-data-dir switch.  The last flag of the same name wins.
      // TODO(tc): It would be nice to remove the flag we don't want, but that
      // sounds risky if we parse differently than CommandLineToArgvW.
      std::wstring new_command_line =
          parsed_command_line.command_line_string();
      CommandLine::AppendSwitchWithValue(&new_command_line,
          switches::kUserDataDir, user_data_dir);
      process_util::LaunchApp(new_command_line, false, false, NULL);
    }

    return ResultCodes::NORMAL_EXIT;
  }

  PrefService* user_prefs = profile->GetPrefs();
  DCHECK(user_prefs);

  // Now that local state and user prefs have been loaded, make the two pref
  // services aware of all our preferences.
  browser::RegisterAllPrefs(user_prefs, local_state);

  // Record last shutdown time into a histogram.
  browser_shutdown::ReadLastShutdownInfo();

  // If the command line specifies 'uninstall' then we need to work here
  // unless we detect another chrome browser running.
  if (parsed_command_line.HasSwitch(switches::kUninstall)) {
    if (already_running) {
      const std::wstring text = l10n_util::GetString(IDS_UNINSTALL_CLOSE_APP);
      const std::wstring caption = l10n_util::GetString(IDS_PRODUCT_NAME);
      win_util::MessageBox(NULL, text, caption,
                           MB_OK | MB_ICONWARNING | MB_TOPMOST);
      return ResultCodes::UNINSTALL_CHROME_ALIVE;
    } else {
      return DoUninstallTasks();
    }
  }

  if (parsed_command_line.HasSwitch(switches::kHideIcons) ||
      parsed_command_line.HasSwitch(switches::kShowIcons)) {
    return HandleIconsCommands(parsed_command_line);
  } else if (parsed_command_line.HasSwitch(switches::kMakeDefaultBrowser)) {
    if (ShellIntegration::SetAsDefaultBrowser()) {
      return ResultCodes::NORMAL_EXIT;
    } else {
      return ResultCodes::SHELL_INTEGRATION_FAILED;
    }
  }

  // Importing other browser settings is done in a browser-like process
  // that exits when this task has finished.
  if (parsed_command_line.HasSwitch(switches::kImport))
    return FirstRun::ImportWithUI(profile, parsed_command_line);

  // When another process is running, use it instead of starting us.
  if (message_window.NotifyOtherProcess(show_command))
    return ResultCodes::NORMAL_EXIT;

  message_window.HuntForZombieChromeProcesses();

  // Do the tasks if chrome has been upgraded while it was last running.
  if (DoUpgradeTasks(parsed_command_line)) {
    return ResultCodes::NORMAL_EXIT;
  }

  message_window.Create();

  // Show the First Run UI if this is the first time Chrome has been run on
  // this computer, or we're being compelled to do so by a command line flag.
  // Note that this be done _after_ the PrefService is initialized and all
  // preferences are registered, since some of the code that the importer
  // touches reads preferences.
  if (is_first_run) {
    // We need to avoid dispatching new tabs when we are doing the import
    // because that will lead to data corruption or a crash. Lock() does that.
    message_window.Lock();
    OpenFirstRunDialog(profile);
    message_window.Unlock();
  }

  // Sets things up so that if we crash from this point on, a dialog will
  // popup asking the user to restart chrome. It is done this late to avoid
  // testing against a bunch of special cases that are taken care early on.
  PrepareRestartOnCrashEnviroment(parsed_command_line);

  // Initialize Winsock.
  net::WinsockInit init;

  // Initialize the DNS prefetch system
  chrome_browser_net::DnsPrefetcherInit dns_prefetch_init(user_prefs);
  chrome_browser_net::DnsPretchHostNamesAtStartup(user_prefs, local_state);

  // Init common control sex.
  INITCOMMONCONTROLSEX config;
  config.dwSize = sizeof(config);
  config.dwICC = ICC_WIN95_CLASSES;
  InitCommonControlsEx(&config);

  win_util::ScopedCOMInitializer com_initializer;

  // Init the RLZ library. This just binds the dll and schedules a task on the
  // file thread to be run sometime later. If this is the first run we record
  // the installation event.
  RLZTracker::InitRlzDelayed(base::DIR_MODULE, is_first_run);

  // Config the network module so it has access to resources.
  net::NetModule::SetResourceProvider(NetResourceProvider);

  // Register our global network handler for chrome-resource:// URLs.
  RegisterURLRequestChromeJob();

  // TODO(brettw): we may want to move this to the browser window somewhere so
  // that if it pops up a dialog box, the user gets it as the child of the
  // browser window instead of a disembodied floating box blocking startup.
  ShellIntegration::VerifyInstallation();

  browser_process->InitBrokerServices(broker_services);

  // Have Chrome plugins write their data to the profile directory.
  PluginService::GetInstance()->SetChromePluginDataDir(profile->GetPath());

  // Initialize the CertStore.
  CertStore::Initialize();

  MetricsService* metrics = NULL;
  if (!parsed_command_line.HasSwitch(switches::kDisableMetrics)) {
    if (parsed_command_line.HasSwitch(switches::kDisableMetricsReporting)) {
      local_state->transient()->SetBoolean(prefs::kMetricsReportingEnabled,
                                           false);
    }
    metrics = browser_process->metrics_service();
    DCHECK(metrics);
    // Start user experience metrics recording, if enabled.
    metrics->SetRecording(local_state->GetBoolean(prefs::kMetricsIsRecording));
  }
  InstallJankometer(parsed_command_line);

  if (parsed_command_line.HasSwitch(switches::kDebugPrint)) {
    browser_process->print_job_manager()->set_debug_dump_path(
        parsed_command_line.GetSwitchValue(switches::kDebugPrint));
  }

  HandleErrorTestParameters(parsed_command_line);

  int result_code = ResultCodes::NORMAL_EXIT;
  if (BrowserInit::ProcessCommandLine(parsed_command_line, L"", local_state,
                                      show_command, true, profile,
                                      &result_code)) {
    MessageLoop::current()->Run(browser_process->accelerator_handler());
  }

  if (metrics)
    metrics->SetRecording(false);  // Force persistent save.

  // browser_shutdown takes care of deleting browser_process, so we need to
  // release it.
  browser_process.release();

  browser_shutdown::Shutdown();

  // The following teardown code will pacify Purify, but is not necessary for
  // shutdown.  Only list methods here that have no significant side effects
  // and can be run in single threaded mode before terminating.
#ifndef NDEBUG  // Don't call these in a Release build: they just waste time.
  // The following should ONLY be called when in single threaded mode. It is
  // unsafe to do this cleanup if other threads are still active.
  // It is also very unnecessary, so I'm only doing this in debug to satisfy
  // purify.
  if (tracking_objects)
    tracked_objects::ThreadData::ShutdownSingleThreadedCleanup();
#endif  // NDEBUG

  return result_code;
}
