#define _CRT_SECURE_NO_WARNINGS
#define CATCH_CONFIG_RUNNER
#include <catch.hpp>

#include <Core/Global.h>
#include <Crypto/SodiumRuntime.h>
#include <Core/Context.h>

#include <Common/Logger.h>
#include <Common/Util/FileUtil.h>
#include <iostream>
#include <cstdlib>

// Test runtime lifetime note (Line 3, ASan finding):
// The previous version defined `static TestRuntime RUNTIME;` and relied on the
// implicit static destructor to call Global::Shutdown(). The Core/Global.cpp
// static strong/weak context holders are destroyed before that static object on
// MSVC (static deinitialization order across translation units), so the
// destructor touched an already-released shared_ptr control block: AddressSanitizer
// reported a heap-use-after-free on every exit, even with zero tests.
//
// The runtime is therefore created as a LOCAL object inside main() below. It is
// destroyed before static destruction, while Global's statics are still alive.
// Global.cpp's production lifetime is intentionally unchanged; the latent
// static-init/deinit hazard is tracked as a separate, unverified finding.
class TestRuntime
{
public:
    TestRuntime()
    {
        FileUtil::RemoveFile(Config::DefaultDataDir(Environment::AUTOMATED_TESTING));

        m_pConfig = Config::Default(Environment::AUTOMATED_TESTING);
        m_pContext = Context::Create(Environment::AUTOMATED_TESTING, m_pConfig);
        Global::Init(m_pContext);
        const char* testLogLevel = std::getenv("GRINPP_TEST_LOG_LEVEL");
        LoggerAPI::Initialize(m_pConfig->GetLogDirectory(),
            testLogLevel != nullptr ? std::string(testLogLevel) : m_pConfig->GetLogLevel());

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
    if (!SodiumRuntime::Initialize()) {
        std::cerr << "Unable to initialize libsodium." << std::endl;
        return 1;
    }
    TestRuntime runtime; // destroyed before static destruction: Global statics still alive
    return Catch::Session().run(argc, argv);
}


