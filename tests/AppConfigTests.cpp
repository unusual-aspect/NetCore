#include <gtest/gtest.h>

#include "AppConfig.hpp"
#include "NetDefaults.hpp"

TEST(AppConfigTest, ParsesServerArguments) {
    char* argv[] = { (char*)"NetServer", (char*)"--db", (char*)"test.db" };
    int argc = 3;

    auto settings = AppConfig::parseServer(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->port, NET_DEFAULT_PORT);
    EXPECT_EQ(settings->db_path, "test.db");
    EXPECT_EQ(settings->bind_host, NET_DEFAULT_BIND);
    EXPECT_FALSE(settings->allow_remote_shutdown);
    EXPECT_FALSE(settings->verbose);
    EXPECT_EQ(settings->access_log_max_rows, kDefaultAccessLogMaxRows);
    EXPECT_FALSE(settings->help_requested);
}

TEST(AppConfigTest, ServerDefaultBindIsLocalhost) {
    char* argv[] = { (char*)"NetServer" };
    int argc = 1;

    auto settings = AppConfig::parseServer(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->bind_host, "127.0.0.1");
}

TEST(AppConfigTest, ServerParsesBindAndHardeningFlags) {
    char* argv[] = {
        (char*)"NetServer",
        (char*)"--bind", (char*)"0.0.0.0",
        (char*)"--allow-remote-shutdown",
        (char*)"--log-retain", (char*)"50",
        (char*)"--verbose"
    };
    int argc = 7;

    auto settings = AppConfig::parseServer(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->bind_host, "0.0.0.0");
    EXPECT_TRUE(settings->allow_remote_shutdown);
    EXPECT_EQ(settings->access_log_max_rows, 50u);
    EXPECT_TRUE(settings->verbose);
}

TEST(AppConfigTest, ServerDetectsHelp) {
    char* argv[] = { (char*)"NetServer", (char*)"--help" };
    int argc = 2;

    auto settings = AppConfig::parseServer(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_TRUE(settings->help_requested);
}

TEST(AppConfigTest, ParsesClientRead) {
    char* argv[] = { (char*)"NetClient", (char*)"--read" };
    int argc = 2;

    auto settings = AppConfig::parseClient(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->opcode, Opcode::Read);
    EXPECT_FALSE(settings->help_requested);
}

TEST(AppConfigTest, ParsesClientSet) {
    char* argv[] = { (char*)"NetClient", (char*)"--set", (char*)"hello" };
    int argc = 3;

    auto settings = AppConfig::parseClient(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->opcode, Opcode::Set);
    EXPECT_EQ(settings->payload, "hello");
}

TEST(AppConfigTest, ClientDefaultsToPromptSet) {
    char* argv[] = { (char*)"NetClient" };
    int argc = 1;

    auto settings = AppConfig::parseClient(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->opcode, Opcode::Set);
    EXPECT_TRUE(settings->prompt_message);
}

TEST(AppConfigTest, ClientDetectsHelp) {
    char* argv[] = { (char*)"NetClient", (char*)"--help" };
    int argc = 2;

    auto settings = AppConfig::parseClient(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_TRUE(settings->help_requested);
}

TEST(AppConfigTest, ClientDetectsShortHelp) {
    char* argv[] = { (char*)"NetClient", (char*)"-h" };
    int argc = 2;

    auto settings = AppConfig::parseClient(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_TRUE(settings->help_requested);
}

TEST(AppConfigTest, ClientParsesShortHost) {
    char* argv[] = { (char*)"NetClient", (char*)"-H", (char*)"10.0.0.1", (char*)"--read" };
    int argc = 4;

    auto settings = AppConfig::parseClient(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->host, "10.0.0.1");
    EXPECT_EQ(settings->opcode, Opcode::Read);
}

TEST(AppConfigTest, ClientParsesShortPort) {
    char* argv[] = { (char*)"NetClient", (char*)"-P", (char*)"8080", (char*)"--read" };
    int argc = 4;

    auto settings = AppConfig::parseClient(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->port, 8080);
    EXPECT_EQ(settings->opcode, Opcode::Read);
}

TEST(AppConfigTest, ServerRejectsPortFlag) {
    char* argv[] = { (char*)"NetServer", (char*)"-P", (char*)"8080" };
    int argc = 3;

    auto settings = AppConfig::parseServer(argc, argv);

    // Port is not a server option — cxxopts rejects unknown flags.
    EXPECT_FALSE(settings.has_value());
}

TEST(AppConfigTest, ServerPortIsAlwaysDefault) {
    char* argv[] = { (char*)"NetServer", (char*)"--bind", (char*)"0.0.0.0" };
    int argc = 3;

    auto settings = AppConfig::parseServer(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ(settings->port, NET_DEFAULT_PORT);
}

TEST(AppConfigTest, ClientParsesVerbose) {
    char* argv[] = { (char*)"NetClient", (char*)"--verbose", (char*)"--read" };
    int argc = 3;

    auto settings = AppConfig::parseClient(argc, argv);

    ASSERT_TRUE(settings.has_value());
    EXPECT_TRUE(settings->verbose);
}
