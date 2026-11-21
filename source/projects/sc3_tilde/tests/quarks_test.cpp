// SPDX-License-Identifier: GPL-3.0-only
// quarks_test <bundle Resources dir> ok|broken|app|appwrong [SuperCollider.app]
//
// Boots the engine as sc3~ configures it (sc3SelectLibrary), with SC's user dirs
// redirected to a temp dir (the real ones are never read).
//   ok:       bundled library, no version check; a user Extension and a Quark (via
//             sclang_conf.yaml includePaths) compile and run.
//   broken:   the Quark has a syntax error; the boot degrades to stock + SCMaxCompat,
//             drops the user classes and reports both exclusions.
//   app:      SC3_APP_PATH=<SuperCollider.app 3.14.x> wins over the bundle; the same
//             user classes compile under the app's library.
//   appwrong: SC3_APP_PATH points at a fake 3.13.0 app; the version check fails the boot.
// One mode per process: the user config is fixed by the first boot of the VM.

#include "../Sc3Engine.h"
#include "SC_Filesystem.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using DirName = SC_Filesystem::DirName;

static int failures = 0;

static void check(bool ok, const std::string &what)
{
	std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
	if (!ok)
		++failures;
}

static void write(const fs::path &p, const std::string &text)
{
	fs::create_directories(p.parent_path());
	std::ofstream(p) << text;
}

static bool contains(const std::string &s, const std::string &part)
{
	return s.find(part) != std::string::npos;
}

// Waits for a line starting with `tag` in the post output; returns the rest of it.
static std::string awaitLine(Sc3Engine &eng, const std::string &tag, int timeoutMs = 20000)
{
	std::string all;
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
	while (std::chrono::steady_clock::now() < end) {
		all += eng.takePost();
		const size_t at = all.find(tag);
		if (at != std::string::npos) {
			const size_t nl = all.find('\n', at);
			if (nl != std::string::npos)
				return all.substr(at + tag.size(), nl - at - tag.size());
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	std::fprintf(stderr, "timeout waiting for '%s'; post was:\n%s\nerror was:\n%s\n", tag.c_str(),
	             all.c_str(), eng.takeError().c_str());
	return "<timeout>";
}

int main(int argc, char **argv)
{
	const std::string mode = argc >= 3 ? argv[2] : "";
	const bool needsApp = mode == "app";
	if (!(mode == "ok" || mode == "broken" || mode == "app" || mode == "appwrong") ||
	    argc != (needsApp ? 4 : 3)) {
		std::fprintf(stderr, "usage: %s <bundle Resources dir> ok|broken|app|appwrong [SuperCollider.app]\n",
		             argv[0]);
		return 2;
	}
	const fs::path res = fs::canonical(argv[1]);

	char tmpl[] = "/tmp/sc3_quarks_test.XXXXXX";
	if (!mkdtemp(tmpl)) {
		std::perror("mkdtemp");
		return 2;
	}
	const fs::path tmp = fs::canonical(tmpl);
	const fs::path config = tmp / "config", support = tmp / "support";
	const fs::path userExt = support / "Extensions", sysExt = tmp / "system-extensions";
	const fs::path quark = support / "downloaded-quarks" / "SC3TestQuark";
	fs::create_directories(sysExt);

	write(userExt / "SC3TestExt.sc",
	      "SC3TestExt { *hello { ^\"ext-ok\" } *viaQuark { ^SC3TestQuark.sum } }\n");
	write(quark / "SC3TestQuark.quark", "(name: \"SC3TestQuark\", version: \"0.1\")\n");
	write(quark / "Classes" / "SC3TestQuark.sc", "SC3TestQuark { *sum { ^[1, 2, 3].sum } }\n");
	if (mode == "broken")
		write(quark / "Classes" / "Broken.sc", "SC3BrokenQuark { *f { ^( } }\n");
	write(config / "sclang_conf.yaml",
	      "includePaths:\n  - " + quark.string() + "\nexcludePaths: []\npostInlineWarnings: false\n");

	// The app SC3_APP_PATH names. appwrong: a 3.13.0 Info.plist over the bundled
	// library, so only the version check can fail the boot.
	fs::path app;
	if (needsApp) {
		app = argv[3];
	} else if (mode == "appwrong") {
		app = tmp / "SuperCollider.app";
		write(app / "Contents" / "Info.plist",
		      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		      "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
		      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
		      "<plist version=\"1.0\"><dict><key>CFBundleShortVersionString</key>"
		      "<string>3.13.0</string></dict></plist>\n");
		fs::create_directories(app / "Contents" / "Resources");
		fs::create_directory_symlink(res / "SCClassLibrary",
		                             app / "Contents" / "Resources" / "SCClassLibrary");
	}
	const fs::path libRes = app.empty() ? res : app / "Contents" / "Resources";

	// Before open(): getDirectory() caches these, and the boot reads them.
	SC_Filesystem &sfs = SC_Filesystem::instance();
	sfs.setDirectory(DirName::UserAppSupport, support);
	sfs.setDirectory(DirName::UserExtension, userExt);
	sfs.setDirectory(DirName::SystemExtension, sysExt);

	unsetenv("SC3_ALLOW_SC_VERSION"); // would skip the check appwrong expects
	if (app.empty())
		unsetenv("SC3_APP_PATH");
	else
		setenv("SC3_APP_PATH", app.c_str(), 1);
	Sc3EngineConfig cfg;
	sc3SelectLibrary(cfg, res.string());
	if (app.empty()) {
		check(cfg.scAppPath.empty() && cfg.classLibraryDir == res.string(),
		      "no SC3_APP_PATH: library from the bundle");
	} else {
		check(cfg.scAppPath == app.string() && cfg.classLibraryDir.empty(),
		      "SC3_APP_PATH wins over the bundle");
	}
	check(cfg.compatDir == (res / "SCMaxCompat").string(), "SCMaxCompat from the bundle");
	// Stock plugins only, so no user .scx from the real ~/Library is loaded.
	cfg.ugenPluginsPath = (libRes / "plugins").string();
	cfg.userConfigDir = config.string();

	Sc3Engine &eng = Sc3Engine::instance();
	check(eng.open(cfg), "open");
	const bool ready = eng.waitReady(120000);
	const Sc3EngineStatus st = eng.status();

	if (mode == "appwrong") {
		check(!ready && st.failed, "boot fails");
		check(contains(st.error, "SuperCollider installation is 3.13.0"),
		      "error names the version (" + st.error + ")");
	} else {
		check(ready && !st.failed, "boot (error: " + st.error + ")");
		check(st.classLibraryDir == libRes.string(), "class library dir: " + st.classLibraryDir);
		check(st.versionNote.empty(), "no version note (" + st.versionNote + ")");
	}

	if (mode == "ok" || mode == "app") {
		check(!st.degraded, "not degraded (" + st.degradeReport + ")");
		eng.evalSilent("(\"SC3T \" ++ [SC3TestExt.hello, SC3TestQuark.sum, SC3TestExt.viaQuark, "
		               "Quarks.isInstalled(\"SC3TestQuark\"), Platform.resourceDir, "
		               "Platform.userExtensionDir].join(\"|\")).postln");
		const std::string want = "ext-ok|6|6|true|" + libRes.string() + "|" + userExt.string();
		const std::string got = awaitLine(eng, "SC3T ");
		check(got == want, "user classes compile and run\n  want " + want + "\n  got  " + got);
	} else if (mode == "broken") {
		check(st.degraded, "degraded");
		check(contains(st.degradeReport, "exclude (includePaths): " + quark.string()),
		      "report names the Quark");
		check(contains(st.degradeReport, "exclude (default dir): " + userExt.string()),
		      "report names the user Extensions dir");
		eng.evalSilent("(\"SC3T \" ++ [\\SC3TestExt.asClass.isNil, \\SC3TestQuark.asClass.isNil, "
		               "\\SCMaxCompatState.asClass.notNil, Platform.resourceDir].join(\"|\")).postln");
		const std::string want = "true|true|true|" + res.string();
		const std::string got = awaitLine(eng, "SC3T ");
		check(got == want, "stock + SCMaxCompat after the fallback\n  want " + want + "\n  got  " + got);
	}

	eng.release();
	fs::remove_all(tmp);
	std::printf("%s: %d failure(s)\n", mode.c_str(), failures);
	return failures ? 1 : 0;
}
