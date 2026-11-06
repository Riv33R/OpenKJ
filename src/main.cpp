/*
 * Copyright (c) 2013-2021 Thomas Isaac Lightburn
 *
 *
 * This file is part of OpenKJ.
 *
 * OpenKJ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <algorithm>
#include <QApplication>
#include "mainwindow.h"
#include <QStyleFactory>
#include <QSplashScreen>
#include <QStringList>
#include <QMessageBox>
#include "settings.h"
#include "idledetect.h"
#include "runguard/runguard.h"
#include "okjversion.h"
#include "okjtranslator.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async_logger.h>
#include <spdlog/async.h>


#ifdef __MINGW32__
#include <streambuf>
struct OkjStreambufCompat {
#if defined(__MINGW64__)
    std::streampos seekpos(std::streampos, std::ios_base::openmode)
        __asm__("_ZNSt15basic_streambufIcSt11char_traitsIcEE7seekposESt4fposIiESt13_Ios_Openmode");
#else
    std::streampos seekpos(std::streampos, std::ios_base::openmode)
        __asm__("__ZNSt15basic_streambufIcSt11char_traitsIcEE7seekposESt4fposIiESt13_Ios_Openmode@12");
#endif
};
__attribute__((used))
std::streampos OkjStreambufCompat::seekpos(std::streampos, std::ios_base::openmode)
{
    return std::streampos(std::streamoff(-1));
}
#endif

Settings settings;
IdleDetect *filter;

//todo: This should be me moved into main after migration to spdlog is complete
//      It's currently only global for use by the QDebug callback
std::shared_ptr<spdlog::async_logger> logger;


void myMessageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    bool loggingEnabled = settings.logEnabled();
    std::string logMsg = msg.toStdString();
    if (context.function) {
        logMsg.append(" [");
        logMsg.append(context.function);
        logMsg.append("]");
    }
    switch (type) {
        case QtDebugMsg:
            if (!loggingEnabled)
                return;
            logger->debug(logMsg);
            break;
        case QtInfoMsg:
            logger->info(logMsg);
            break;
        case QtWarningMsg:
            logger->warn(logMsg);
            break;
        case QtCriticalMsg:
            logger->critical(logMsg);
            break;
        case QtFatalMsg:
            logger->critical(logMsg);
          //  abort();
    }
}

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    // Prevent Windows from showing system error message boxes for missing/unsupported DLLs during dynamic plugin scanning
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // Ensure the application directory is first in DLL search path and PATH environment variable
    // so any child processes (like gst-plugin-scanner.exe) and plugins find the local runtime DLLs.
    wchar_t exePathBuf[MAX_PATH];
    if (GetModuleFileNameW(NULL, exePathBuf, MAX_PATH) > 0) {
        QString fullExePath = QString::fromWCharArray(exePathBuf);
        QString nativeAppDir = QDir::toNativeSeparators(QFileInfo(fullExePath).absolutePath());
        SetDllDirectoryW(reinterpret_cast<LPCWSTR>(nativeAppDir.utf16()));
        QString currentPath = QString::fromLocal8Bit(qgetenv("PATH"));
        if (!currentPath.startsWith(nativeAppDir, Qt::CaseInsensitive)) {
            QString newPath = nativeAppDir + ";" + currentPath;
            qputenv("PATH", newPath.toLocal8Bit());
        }
    }
#endif
    QString logDir = settings.logDir();
    QDir dir;
    QString logFilePath;
    QString filename = "openkj-debug-" + QDateTime::currentDateTime().toString("yyyy-MM-dd") + ".log";
    dir.mkpath(logDir);
    logFilePath = logDir + QDir::separator() + filename;

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFilePath.toLocal8Bit().toStdString(), false);
    spdlog::init_thread_pool(8192, 2);
    std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
    logger = std::make_shared<spdlog::async_logger>("logger", sinks.begin(), sinks.end(), spdlog::thread_pool(),
                                                    spdlog::async_overflow_policy::block);
    spdlog::register_logger(logger);
    logger->set_level(spdlog::level::trace);
    spdlog::flush_every(std::chrono::seconds(1));
    logger->flush_on(spdlog::level::warn);

    auto consoleLogLevel = settings.getConsoleLogLevel();
    auto fileLogLevel = settings.getFileLogLevel();

    switch (consoleLogLevel) {
        case Settings::LOG_LEVEL_CRITICAL:
            console_sink->set_level(spdlog::level::critical);
            break;
        case Settings::LOG_LEVEL_ERROR:
            console_sink->set_level(spdlog::level::err);
            break;
        case Settings::LOG_LEVEL_WARNING:
            console_sink->set_level(spdlog::level::warn);
            break;
        case Settings::LOG_LEVEL_INFO:
            console_sink->set_level(spdlog::level::info);
            break;
        case Settings::LOG_LEVEL_DEBUG:
            console_sink->set_level(spdlog::level::debug);
            break;
        case Settings::LOG_LEVEL_TRACE:
            console_sink->set_level(spdlog::level::trace);
            break;
        default:
            console_sink->set_level(spdlog::level::off);
    }
    switch (fileLogLevel) {
        case Settings::LOG_LEVEL_CRITICAL:
            file_sink->set_level(spdlog::level::critical);
            break;
        case Settings::LOG_LEVEL_ERROR:
            file_sink->set_level(spdlog::level::err);
            break;
        case Settings::LOG_LEVEL_WARNING:
            file_sink->set_level(spdlog::level::warn);
            break;
        case Settings::LOG_LEVEL_INFO:
            file_sink->set_level(spdlog::level::info);
            break;
        case Settings::LOG_LEVEL_DEBUG:
            file_sink->set_level(spdlog::level::debug);
            break;
        case Settings::LOG_LEVEL_TRACE:
            file_sink->set_level(spdlog::level::trace);
            break;
        default:
            file_sink->set_level(spdlog::level::off);
    }
    if (file_sink->level() > console_sink->level())
        logger->set_level(file_sink->level());
    else
        logger->set_level(console_sink->level());

    logger->set_level(spdlog::level::trace);
    console_sink->set_pattern("[%^%l%$] %v");
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");

    logger->info("OpenKJ version {} starting up", OKJ_VERSION_STRING);


    //QLoggingCategory::setFilterRules("*.debug=true");
    qInstallMessageHandler(myMessageOutput);
    QApplication a(argc, argv);

    OkjTranslator translator;
    QTranslator qtTranslator;
    if (settings.language() == "ru") {
        if (qtTranslator.load("qt_ru", QCoreApplication::applicationDirPath() + "/translations")) {
            a.installTranslator(&qtTranslator);
        }
        if (translator.loadTs(":/translations/openkj_ru.ts")) {
            a.installTranslator(&translator);
            logger->info("Loaded Russian UI translation");
        }
    }

#ifdef Q_OS_WIN
    if (qgetenv("GST_PLUGIN_SYSTEM_PATH").isEmpty()) {
        QString appDir = QCoreApplication::applicationDirPath();
        QStringList candidateRoots;
        // Priority 1: Portable bundled GStreamer inside application directory
        candidateRoots << appDir;

        // Priority 2: Standard GStreamer environment variables
#if QT_POINTER_SIZE == 4
        QString gstEnv = QString::fromLocal8Bit(qgetenv("GSTREAMER_1_0_ROOT_MINGW_X86"));
        if (!gstEnv.isEmpty()) candidateRoots << gstEnv;
        gstEnv = QString::fromLocal8Bit(qgetenv("GSTREAMER_1_0_ROOT_X86"));
        if (!gstEnv.isEmpty()) candidateRoots << gstEnv;
#else
        QString gstEnv = QString::fromLocal8Bit(qgetenv("GSTREAMER_1_0_ROOT_MINGW_X86_64"));
        if (!gstEnv.isEmpty()) candidateRoots << gstEnv;
        gstEnv = QString::fromLocal8Bit(qgetenv("GSTREAMER_1_0_ROOT_X86_64"));
        if (!gstEnv.isEmpty()) candidateRoots << gstEnv;
#endif

        // Priority 3: Dynamic ProgramFiles detection without hardcoded drive letters
        QString progFiles = QString::fromLocal8Bit(qgetenv("ProgramFiles"));
        QString progFilesX86 = QString::fromLocal8Bit(qgetenv("ProgramFiles(x86)"));
        QString sysDrive = QString::fromLocal8Bit(qgetenv("SystemDrive"));
        if (sysDrive.isEmpty()) sysDrive = "C:";

#if QT_POINTER_SIZE == 4
        if (!progFilesX86.isEmpty()) {
            candidateRoots << progFilesX86 + "/gstreamer/1.0/mingw_x86";
        }
        if (!progFiles.isEmpty()) {
            candidateRoots << progFiles + "/gstreamer/1.0/mingw_x86";
        }
        candidateRoots << sysDrive + "/gstreamer/1.0/mingw_x86";
#else
        if (!progFiles.isEmpty()) {
            candidateRoots << progFiles + "/gstreamer/1.0/mingw_x86_64";
        }
        candidateRoots << sysDrive + "/gstreamer/1.0/mingw_x86_64";
#endif

        for (const QString &root : candidateRoots) {
            QString pluginPath = root + "/lib/gstreamer-1.0";
            if (QDir(pluginPath).exists()) {
                qputenv("GST_PLUGIN_SYSTEM_PATH", QDir::toNativeSeparators(pluginPath).toUtf8());
                QString localScanner = root + "/gst-plugin-scanner.exe";
                QString scannerPath = root + "/libexec/gstreamer-1.0/gst-plugin-scanner.exe";
                if (QFileInfo::exists(localScanner)) {
                    qputenv("GST_PLUGIN_SCANNER", QDir::toNativeSeparators(localScanner).toUtf8());
                } else if (QFileInfo::exists(scannerPath)) {
                    qputenv("GST_PLUGIN_SCANNER", QDir::toNativeSeparators(scannerPath).toUtf8());
                }
                break;
            }
        }
    }
#endif

#ifdef MAC_OVERRIDE_GST
    // This points GStreamer paths to the framework contained in the app bundle.  Not needed on brew installs.
    QString appDir = QCoreApplication::applicationDirPath();
    qInfo() << "Application dir: " << appDir;
    appDir.remove(appDir.length() - 5, 5);
    qputenv("GST_PLUGIN_SYSTEM_PATH", QString(appDir + "Frameworks/GStreamer.framework/Versions/Current/lib/gstreamer-1.0").toLocal8Bit());
    qputenv("GST_PLUGIN_SCANNER", QString(appDir + "Frameworks/GStreamer.framework/Versions/Current/libexec/gstreamer-1.0/gst-plugin-scanner").toLocal8Bit());
    qputenv("GTK_PATH", QString(appDir + "Frameworks/GStreamer.framework/Versions/Current/").toLocal8Bit());
    qputenv("GIO_EXTRA_MODULES", QString(appDir + "Frameworks/GStreamer.framework/Versions/Current/lib/gio/modules").toLocal8Bit());
    qWarning() << "MacOS detected, changed GST env vars to point to the bundled framework";
    qWarning() << qgetenv("GST_PLUGIN_SYSTEM_PATH") << endl << qgetenv("GST_PLUGIN_SCANNER") << endl << qgetenv("GTK_PATH") << endl << qgetenv("GIO_EXTRA_MODULES") << endl;
#endif

    a.installEventFilter(filter);
    qputenv("GST_DEBUG", "*:3");
    QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    if (settings.theme() == 1) {
        QPalette palette;
        QApplication::setStyle(QStyleFactory::create("Fusion"));
        palette.setColor(QPalette::Window, QColor(53, 53, 53));
        palette.setColor(QPalette::WindowText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(127, 127, 127));
        palette.setColor(QPalette::Base, QColor(42, 42, 42));
        palette.setColor(QPalette::AlternateBase, QColor(66, 66, 66));
        palette.setColor(QPalette::ToolTipBase, Qt::white);
        palette.setColor(QPalette::ToolTipText, QColor(53, 53, 53));
        palette.setColor(QPalette::Text, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
        palette.setColor(QPalette::Dark, QColor(35, 35, 35));
        palette.setColor(QPalette::Shadow, QColor(20, 20, 20));
        palette.setColor(QPalette::Button, QColor(53, 53, 53));
        palette.setColor(QPalette::ButtonText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
        palette.setColor(QPalette::BrightText, Qt::red);
        palette.setColor(QPalette::Link, QColor(42, 130, 218));
        palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
        palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(80, 80, 80));
        palette.setColor(QPalette::HighlightedText, Qt::white);
        palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor(127, 127, 127));
        QApplication::setPalette(palette);
    } else if (settings.theme() == 2) {
        QApplication::setStyle(QStyleFactory::create("Fusion"));
    }
//    else
//    {
//
//    }
    QApplication::setFont(settings.applicationFont(), "QWidget");
    QApplication::setFont(settings.applicationFont(), "QMenu");
    QApplication::setFont(settings.applicationFont(), "QAction");

    // RunGuard seems to be broken by flatpak, ignore it if running in a flatpak sandbox
    logger->info("App dir path {}", QCoreApplication::applicationDirPath().toStdString());
    if (QCoreApplication::applicationDirPath() == "/app/bin") {
        logger->info("RunGuard disabled due to flatpak sandbox");
    } else {
#ifdef Q_OS_WIN
        HANDLE hSingleInstanceMutex = CreateMutexW(NULL, TRUE, L"Local\\OpenKJ_SingleInstanceProtector");
        if (hSingleInstanceMutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(hSingleInstanceMutex);
            QMessageBox msgBox;
            msgBox.setText(QCoreApplication::translate("main", "OpenKJ is already running!"));
            msgBox.setInformativeText(
                    QCoreApplication::translate("main", "In order to protect the database, you can only run one instance of OpenKJ at a time.\nExiting now."));
            msgBox.setIcon(QMessageBox::Critical);
            msgBox.exec();
            return 1;
        }
#else
        logger->debug("Checking for other instances");
        RunGuard guard("SharedMemorySingleInstanceProtectorOpenKJ");
        if (!guard.tryToRun()) {
            QMessageBox msgBox;
            msgBox.setText(QCoreApplication::translate("main", "OpenKJ is already running!"));
            msgBox.setInformativeText(
                    QCoreApplication::translate("main", "In order to protect the database, you can only run one instance of OpenKJ at a time.\nExiting now."));
            msgBox.setIcon(QMessageBox::Critical);
            msgBox.exec();
            return 1;
        }
#endif
    }
    if (!settings.lastStartupOk()) {
        QMessageBox msgBox;
        msgBox.setText(QCoreApplication::translate("main", "OpenKJ appears to have failed to startup on the last run."));
        msgBox.setInformativeText(QCoreApplication::translate("main", "Would you like to attempt to recover by loading safe settings?"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        msgBox.setDefaultButton(QMessageBox::Yes);
        if (msgBox.exec() == QMessageBox::Yes)
            settings.setSafeStartupMode(true);
        else
            settings.setSafeStartupMode(false);
    }
#ifdef Q_OS_DARWIN
    if (settings.lastRunVersion() != OKJ_VERSION_STRING)
        settings.setSafeStartupMode(true);
#endif
    settings.setLastRunVersion(OKJ_VERSION_STRING);
    settings.setStartupOk(false);
    MainWindow w;
    w.show();
    return QApplication::exec();
}
