// Diagnostic (not a fuzz target): reproduces the test-runtime static
// init/shutdown ownership of Context/Config and prints shared_ptr use counts.
// Used to root-cause the ASan heap-use-after-free at TestRuntime::~TestRuntime,
// which reproduces even when no test cases run.
#include <cstdio>

#include <Core/Global.h>
#include <Core/Context.h>
#include <Common/Logger.h>
#include <Common/Util/FileUtil.h>

class TestRuntime
{
public:
    TestRuntime()
    {
        std::printf("[ctor] begin\n"); std::fflush(stdout);
        FileUtil::RemoveFile(Config::DefaultDataDir(Environment::AUTOMATED_TESTING));
        m_pConfig = Config::Default(Environment::AUTOMATED_TESTING);
        m_pContext = Context::Create(Environment::AUTOMATED_TESTING, m_pConfig);
        std::printf("[ctor] after create: m_pContext.use_count=%ld global.use_count=%ld\n",
            (long)m_pContext.use_count(), (long)Global::GetContext().use_count()); std::fflush(stdout);
        Global::Init(m_pContext);
        std::printf("[ctor] after Global::Init: m_pContext.use_count=%ld global.use_count=%ld\n",
            (long)m_pContext.use_count(), (long)Global::GetContext().use_count()); std::fflush(stdout);
        LoggerAPI::Initialize(m_pConfig->GetLogDirectory(), m_pConfig->GetLogLevel());
    }

    ~TestRuntime()
    {
        {
            const Context::Ptr g = Global::GetContext();
            std::printf("[dtor] step1 with weak lock: m_pContext=%ld weak_lock=%ld\n",
                (long)m_pContext.use_count(), (long)g.use_count()); std::fflush(stdout);
        }
        std::printf("[dtor] step2 before Shutdown: m_pContext=%ld\n", (long)m_pContext.use_count()); std::fflush(stdout);
        Global::Shutdown();
        std::printf("[dtor] step3 after Shutdown: m_pContext=%ld\n", (long)m_pContext.use_count()); std::fflush(stdout);
    }

private:
    ConfigPtr m_pConfig;
    Context::Ptr m_pContext;
};

static TestRuntime RUNTIME;

int main()
{
    std::printf("[main]\n");
    std::fflush(stdout);
    return 0;
}
