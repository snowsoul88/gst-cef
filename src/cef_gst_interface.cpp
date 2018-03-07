#include <ctime>
#include <iostream>
#include <list>
#include <glib.h>
#include <Windows.h>
#include <cwchar>

#include <include/cef_app.h>
#include <include/cef_client.h>
#include <include/cef_render_handler.h>
#include "include/cef_sandbox_win.h"
#include <include/wrapper/cef_helpers.h>

#include "cef.h"
#include "cef_app.h"

namespace
{
static gint loop_live = 0;
static CefRefPtr<Browser> app;
static GMutex cef_start_mutex;
static GCond cef_start_cond;
static guint browser_loop_index = 0;
} // namespace

static bool doStart(gpointer data)
{
  GST_DEBUG("doStart");

  // Provide CEF with command-line arguments.
  struct gstCb *cb = (struct gstCb *)data;
  CefMainArgs main_args(GetModuleHandle(NULL));

  // Specify CEF global settings here.
  CefSettings settings;

  HMODULE hModule = GetModuleHandleW(NULL);
  WCHAR path[MAX_PATH + 50];
  GetModuleFileNameW(hModule, path, MAX_PATH);
  int slash_index = 0;
  for (int i = 0; i < MAX_PATH; i++)
  {
    if (path[i] == L"\\"[0])
    {
      slash_index = i;
    }
  }
  path[slash_index + 1] = L"\0"[0];
  std::wcout << L"Resource Directory Path: " << path << std::endl;
  CefString(&settings.resources_dir_path).FromWString(path);
  std::wstring subprocess = L"\\subprocess";
  for (int i = slash_index; i < slash_index + 12; i++)
  {
    path[i] = subprocess[i - slash_index];
  }
  std::wcout << L"Subprocess Path: " << path << std::endl;
  CefString(&settings.browser_subprocess_path).FromWString(path);

  settings.windowless_rendering_enabled = true;
  settings.no_sandbox = true;

  // settings.log_severity = LOGSEVERITY_VERBOSE;
  settings.multi_threaded_message_loop = false;

  // Browser implements application-level callbacks for the browser process.
  // It will create the first browser instance in OnContextInitialized() on the 
  // browser UI thread after the CEF has initialized.
  app = new Browser(cb->gstCef, cb->push_frame, cb->url, cb->width, cb->height, cb->initialization_js);
  g_free(cb);

  // Initialize CEF for the browser process.
  GST_DEBUG("CefInitialize");
  CefInitialize(main_args, settings, app.get(), NULL);

  g_mutex_lock(&cef_start_mutex);
  g_cond_signal(&cef_start_cond);
  g_mutex_unlock(&cef_start_mutex);
  return false;
}

static bool doOpen(gpointer data)
{
  GST_INFO("doOpen");
  struct gstCb *cb = (struct gstCb *)data;
  app.get()->Open(cb->gstCef, cb->push_frame, cb->url, cb->width, cb->height, cb->initialization_js);
  g_free(cb);
  return false;
}

static bool doWork(gpointer data)
{
  if (g_atomic_int_get(&loop_live))
  {
    CefDoMessageLoopWork();
  }
  return false;
}

static bool doShutdown(gpointer data)
{
  GST_LOG("doShutdown");
  CefShutdown();
  return false;
}

void browser_loop(gpointer args)
{
  GST_INFO("Entering browser loop.");
  if (app)
  {
    GST_INFO("have app");
    g_idle_add((GSourceFunc)doOpen, args);
    return;
  }
  browser_loop_index++;

  g_atomic_int_set(&loop_live, 1);
  GST_INFO("Adding doStart to Bus.");
  g_idle_add((GSourceFunc)doStart, args);

  // Add doWork to the bus.  We don't really have a main function which is why
  // we need to do doWork here.  This is less efficient than
  // CefRunMessageLoop(), but we need the UI thread unblocked.
  while (g_atomic_int_get(&loop_live))
  {
    Sleep(32);
    g_idle_add((GSourceFunc)doWork, NULL);
  }
  GST_INFO("MessageLoop Ended");
}

void doOpenBrowser(gpointer args)
{
  GST_INFO("doOpenBrowser");
  g_mutex_lock(&cef_start_mutex);
  while (!app)
  {
    g_cond_wait(&cef_start_cond, &cef_start_mutex);
  }
  g_mutex_unlock(&cef_start_mutex);
  g_idle_add((GSourceFunc)doOpen, args);
}

void open_browser(gpointer args)
{
  g_thread_new("open_browser", (GThreadFunc)doOpenBrowser, args);
}

static bool doSetSize(void *args)
{
  gstSizeArgs *sizeArgs = (gstSizeArgs *)args;
  int width = sizeArgs->width;
  int height = sizeArgs->height;
  void *gstCef = sizeArgs->gstCef;
  g_free(args);
  GST_INFO("set size to %u by %u", width, height);

  if (!app)
  {
    return false;
  }

  app->SetSize(gstCef, width, height);
  return false;
}

void set_size(void *args)
{
  GST_INFO("set_size");
  g_idle_add((GSourceFunc)doSetSize, args);
}

static bool doSetHidden(void *args)
{
  GST_DEBUG("doSetHidden");
  gstHiddenArgs *hiddenArgs = (gstHiddenArgs *)args;
  bool hidden = hiddenArgs->hidden;
  void *gstCef = hiddenArgs->gstCef;
  g_free(args);

  if (!app)
  {
    return false;
  }

  app->SetHidden(gstCef, hidden);
  return false;
}

static bool doExecuteJS(void *args)
{
	GST_DEBUG("doExecuteJS");
    struct gstExecuteJSArgs *exec_js_args = (gstExecuteJSArgs* )args;
	void *gstCef = exec_js_args->gstCef;
	char* js = exec_js_args->js;
	g_free(args);

	if (!app)
	{
		g_free(js);
		return false;
	}

	app->ExecuteJS(gstCef, js);
	return false;
}

void set_hidden(void *args)
{
  GST_INFO("set_hidden");
  g_idle_add((GSourceFunc)doSetHidden, args);
}

void execute_js(void *args)
{
	GST_INFO("Adding doExecuteJS to work loop");
	g_idle_add((GSourceFunc)doExecuteJS, args);
}

bool doClose(gpointer args)
{
  GST_INFO("doClose");
  if (app)
  {
    app->CloseBrowser(args, true);
  }
  else
  {
    GST_ERROR("ERROR: no app");
  }
  return false;
}

void close_browser(gpointer args)
{
  g_idle_add((GSourceFunc)doClose, args);
}

void shutdown_browser()
{
  GST_WARNING("shutdown browser");
  g_atomic_int_set(&loop_live, 0);
  Sleep(2000);
  g_idle_add((GSourceFunc)doShutdown, NULL);
}