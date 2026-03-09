#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <stdexcept>
#include <macgyver/Exception.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


/**
 *  Starts a background process with the given command and arguments.
 *  The process's output is redirected to the specified log file.
 */
pid_t start_background_process(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& log_file)
try
{
    pid_t pid = fork();
    if (pid < 0)
    {
        throw std::runtime_error("Failed to fork process");
    }
    else if (pid == 0)
    {
        // In child process
        std::vector<char*> c_args;
        c_args.push_back(const_cast<char*>(command.c_str()));
        for (const auto& arg : args)
        {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);

        // Redirect stdout and stderr to log file
        freopen(log_file.c_str(), "a", stdout);
        freopen(log_file.c_str(), "a", stderr);

        execvp(command.c_str(), c_args.data());
        // If execvp returns, it must have failed
        std::cerr << "Failed to execute command: " << command << std::endl;
        exit(1);
    }
    // In parent process, return child's PID
    return pid;
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to start background process: " + command) << std::endl;
    throw; // Rethrow to allow handling in main
}

/**
 *  Get TCP/IP port which specified process is listening on.
 *
 *  This is done by from /usr/bin/ss output REGEX parsing.
 *  Ignore also UDP ports, as they are not used in this test.
 */
int get_process_port(pid_t pid)
try
{
    std::string command = "ss -lntp 2>/dev/null | grep " + std::to_string(pid) + " | grep -v udp";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe)
    {
        throw std::runtime_error("Failed to run command: " + command);
    }

    char buffer[128];
    int port = -1;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        std::string line(buffer);
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos)
        {
            size_t space_pos = line.find(' ', colon_pos);
            if (space_pos != std::string::npos)
            {
                port = std::stoi(line.substr(colon_pos + 1, space_pos - colon_pos - 1));
                break;
            }
        }
    }
    pclose(pipe);

    if (port == -1)
    {
        throw std::runtime_error("Failed to find listening port for PID: " + std::to_string(pid));
    }
    return port;
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to get process port for PID: " + std::to_string(pid)) << std::endl;
    throw; // Rethrow to allow handling in main
}

/**
 *   Start smartmet backend processes and return their PIDs and ports.
 */
std::vector<std::pair<pid_t, int>> start_backends(
    const std::vector<std::string>& config_files)
try
{
    int counter = 0;
    std::vector<std::pair<pid_t, int>> backends;
    for (const auto& config : config_files)
    {
        pid_t pid = start_background_process(
            "/usr/sbin/smartmetd",
            {
                "--configfile", config,
                "--port=0" // Let the backend choose an available port
            },
            "log/backend" + std::to_string(counter++) + ".log");
        sleep(1); // Give the process some time to start and listen on the port
        int port = get_process_port(pid);
        backends.emplace_back(pid, port);
        std::cout << "Started backend with PID: " << pid << " on port: " << port << std::endl;
    }
    return backends;
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to start backend processes") << std::endl;
    throw; // Rethrow to allow handling in main
}

void stop_backends(const std::vector<std::pair<pid_t, int>>& backends)
try
{
    for (const auto& [pid, port] : backends)
    {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        std::cout << "Stopped backend with PID: " << pid << " on port: " << port << std::endl;
    }
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to stop backend processes      ") << std::endl;
}

std::pair<pid_t, int> start_frontend(const std::string& config_file)
try
{
    pid_t pid = start_background_process(
        "/usr/sbin/smartmetd",
        {
            "--configfile", config_file,
            "--port=0" // Let the frontend choose an available port
        },
        "log/frontend.log");
    sleep(1); // Give the process some time to start and listen on the port
    int port = get_process_port(pid);
    std::cout << "Started frontend with PID: " << pid << " on port: " << port << std::endl;
    return {pid, port};
}
catch (...)
{
    std::cout << Fmi::Exception(BCP, "Failed to start frontend process") << std::endl;
    throw; // Rethrow to allow handling in main
}

int main()
{
    std::vector<std::pair<pid_t, int>> backends;
    pid_t frontend_pid = 1;
    int frontend_port = -1;

    // Abort test if execution last for more than 5 minutes to avoid hanging indefinitely
    alarm(300); // 300 seconds = 5 minutes

    try
    {
        // Create log directory if it doesn't exist
        std::filesystem::create_directories("log");

        // Start backend processes
        std::vector<std::string> backend_configs = {
            "cnf/reactor_backend1.conf",
            "cnf/reactor_backend2.conf"
        };
        backends = start_backends(backend_configs);

        // Start frontend process
        std::tie(frontend_pid, frontend_port) = start_frontend("cnf/reactor_frontend.conf");

        // Here you would add code to run your tests against the frontend using the frontend_port

        // Stop all processes after tests are done
        kill(frontend_pid, SIGTERM);
        waitpid(frontend_pid, nullptr, 0);
        std::cout << "Stopped frontend with PID: " << frontend_pid << " on port: " << frontend_port << std::endl;

        stop_backends(backends);

        return 0;
    }
    catch (...)
    {
        std::cout << Fmi::Exception(BCP, "An error occurred during test execution") << std::endl;
        try
        {
            std::cout << "Failed: cleaning up processes..." << std::endl;
            stop_backends(backends);
            if (frontend_pid > 1)
            {
                kill(frontend_pid, SIGTERM);
                waitpid(frontend_pid, nullptr, 0);
                std::cout << "Stopped frontend with PID: " << frontend_pid << " on port: " << frontend_port << std::endl;
            }
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
        }
        return 1;
    }
}
