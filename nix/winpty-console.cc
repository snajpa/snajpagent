// SPDX-License-Identifier: GPL-2.0-only
// WinPTY console algorithms; process ownership and authenticated pipes stay in C.
#include <windows.h>
#include <stdint.h>
#include <memory>
#include <stdexcept>
#include "agent/ConsoleInput.h"
#include "agent/DsrSender.h"
#include "agent/EventLoop.h"
#include "agent/NamedPipe.h"
#include "agent/Scraper.h"
#include "agent/Terminal.h"
#include "agent/Win32Console.h"
#include "agent/Win32ConsoleBuffer.h"
#include "shared/WinptyAssert.h"

static HANDLE owned_job;

void agentShutdown()
{
    if (owned_job)
        TerminateJobObject(owned_job, 125);
    ExitProcess(125);
}

void agentAssertFail(const char *, int, const char *) { agentShutdown(); }

static BOOL WINAPI ignore_interrupt(DWORD event) { return event == CTRL_C_EVENT; }

class Console : public EventLoop, public DsrSender {
    Win32Console console;
    NamedPipe *input, *output;
    std::unique_ptr<Scraper> scraper;
    std::unique_ptr<ConsoleInput> keys;
    HANDLE job, control;
    bool draining = false;

    void scrape(COORD size = {})
    {
        // Freeze throughout a scrape, including a blocked output drain, so
        // subsequent child output cannot overwrite uncollected console cells.
        Win32Console::FreezeGuard guard(console, true);
        auto buffer = Win32ConsoleBuffer::openConout();
        ConsoleScreenBufferInfo info;
        if (size.X && size.Y) {
            scraper->resizeWindow(*buffer, Coord(size.X, size.Y), info);
            INPUT_RECORD event = {};
            event.EventType = WINDOW_BUFFER_SIZE_EVENT;
            event.Event.WindowBufferSizeEvent.dwSize = info.dwSize;
            DWORD count;
            if (!WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &event, 1, &count) || count != 1)
                throw std::runtime_error("cannot notify console resize");
        } else
            scraper->scrapeBuffer(*buffer, info);
        keys->setMouseWindowRect(info.windowRect());
    }

    void onPollTimeout() override
    {
        DWORD available = 0;
        if (!PeekNamedPipe(control, nullptr, 0, nullptr, &available, nullptr)) {
            TerminateJobObject(job, 125);
            shutdown();
            return;
        }
        if (available >= sizeof(uint32_t) * 2) {
            uint32_t size[2];
            DWORD count;
            if (!ReadFile(control, size, sizeof(size), &count, nullptr) ||
                count != sizeof(size) || !size[0] || !size[1] ||
                size[0] > MAX_CONSOLE_WIDTH || size[1] > MAX_CONSOLE_HEIGHT)
                throw std::runtime_error("invalid console dimensions");
            scrape(COORD{static_cast<SHORT>(size[0]), static_cast<SHORT>(size[1])});
        }
        if (!draining) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info;
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                            &info, sizeof(info), nullptr))
                throw std::runtime_error("cannot query console job");
            draining = !info.ActiveProcesses;
            keys->updateInputFlags();
            keys->flushIncompleteEscapeCode();
            scrape();
        }
        if (output->isClosed() || (draining && !output->bytesToSend())) {
            output->closePipe();
            shutdown();
        }
    }

    void onPipeIo(NamedPipe &pipe) override
    {
        if (&pipe == input && !draining)
            keys->writeInput(input->readAllToString());
    }

public:
    Console(HANDLE in, HANDLE out, HANDLE ctl, HANDLE owned, COORD size) :
        job(owned), control(ctl)
    {
        input = &createNamedPipe();
        HANDLE duplicate;
        if (!DuplicateHandle(GetCurrentProcess(), in, GetCurrentProcess(), &duplicate,
                              0, FALSE, DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("cannot duplicate console input pipe");
        input->adopt(duplicate, NamedPipe::OpenMode::Reading);
        output = &createNamedPipe();
        if (!DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &duplicate,
                              0, FALSE, DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("cannot duplicate console output pipe");
        output->adopt(duplicate, NamedPipe::OpenMode::Writing);
        auto buffer = Win32ConsoleBuffer::openConout();
        // WinPTY's Mark probe distinguishes classic and modern conhost.
        auto info = buffer->bufferInfo();
        const Coord position(info.srWindow.Right, info.srWindow.Bottom);
        buffer->setCursorPosition(position);
        console.setFreezeUsesMark(true);
        console.setFrozen(true);
        console.setNewW10(buffer->cursorPosition() == position);
        console.setFrozen(false);
        console.setFreezeUsesMark(false);
        buffer->setCursorPosition(Coord(0, 0));
        std::unique_ptr<Terminal> terminal(new Terminal(*output, false, true));
        scraper.reset(new Scraper(console, *buffer, std::move(terminal), Coord(size.X, size.Y)));
        keys.reset(new ConsoleInput(GetStdHandle(STD_INPUT_HANDLE), 0, *this, console));
        SetConsoleCtrlHandler(nullptr, FALSE);
        SetConsoleCtrlHandler(ignore_interrupt, TRUE);
        setPollInterval(25);
    }

    void sendDsr() override { output->write("\x1b[6n"); }
};

extern "C" void *snag_legacy_console_open(HANDLE input, HANDLE output,
                                          HANDLE control, HANDLE job, COORD size)
{
    owned_job = job;
    try {
        return new Console(input, output, control, job, size);
    } catch (...) {
        TerminateJobObject(job, 125);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

extern "C" int snag_legacy_console_run(void *opaque)
{
    std::unique_ptr<Console> console(static_cast<Console *>(opaque));
    try {
        console->run();
        return 0;
    } catch (...) {
        TerminateJobObject(owned_job, 125);
        return -1;
    }
}

extern "C" void snag_legacy_console_free(void *opaque)
{
    delete static_cast<Console *>(opaque);
}
