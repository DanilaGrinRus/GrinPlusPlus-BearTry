// ASan "TestsMini" entry point — harness-owned replacement for tests/src/TestMain.cpp.
//
// Why this file exists:
// The ordinary TestMain.cpp defines `static TestRuntime RUNTIME;`. MSVC destroys
// the static Global context holders in src/Core/Global.cpp (SHARED_CONTEXT /
// GLOBAL_CONTEXT) BEFORE the destructor of that static object, so
// TestRuntime::~TestRuntime() calls Global::Shutdown() on already-destroyed
// shared_ptr/weak_ptr storage. ASan reproduces the resulting heap-use-after-free
// on every run, even when no test case executes (see
// evidence/line3/asan-testsmini-*). That is a real static-deinitialization-order
// finding about TestMain/Global and is reported separately.
//
// For ASan coverage of the *production* code under test we must not let the
// harness die on that test-runtime teardown bug, and we must not silently patch
// production/test code from line 3. This entry point therefore constructs the
// same runtime as a LOCAL object inside main(): Global's static objects are still
// alive when the destructor runs (before static destruction), so the teardown is
// well-defined. Test cases, filters and the isolated HOME/TMP setup are exactly
// the same as the ordinary Tests.exe.
#define CATCH_CONFIG_RUNNER
#include <catch.hpp>

#include <Core/Global.h>
#include <Core/Context.h>

#include <Common/Logger.h>
#include <Common/Util/FileUtil.h>
#include <iostream>

class TestRuntime
{
public:
    TestRuntime()
    {
        FileUtil::RemoveFile(Config::DefaultDataDir(Environment::AUTOMATED_TESTING));

        m_pConfig = Config::Default(Environment::AUTOMATED_TESTING);
        m_pContext = Context::Create(Environment::AUTOMATED_TESTING, m_pConfig);
        Global::Init(m_pContext);
        LoggerAPI::Initialize(m_pConfig->GetLogDirectory(), m_pConfig->GetLogLevel());

        std::cout
            << "Test Runtime Initialized." << std::endl
            << "Running Tests..." << std::endl;
    }

    ~TestRuntime()
    {
        Global::Shutdown();
    }

private:
    ConfigPtr m_pConfig;
    Context::Ptr m_pContext;
};

int main(int argc, char** argv)
{
    TestRuntime runtime; // destroyed before static destruction: Global statics still alive
    const int result = Catch::Session().run(argc, argv);
    return result;
}
