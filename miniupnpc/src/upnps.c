#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")

#include <windows.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>

#include "../include/miniupnpc.h"
#include "../include/upnpcommands.h"
#include "../include/upnperrors.h"
#include "../include/upnpdev.h"

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")

#define SERVICE_NAME "UPnPSrv"
char g_LogFilePath[MAX_PATH] = {0};

SERVICE_STATUS        g_ServiceStatus = {0};
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE                g_ServiceStopEvent = INVALID_HANDLE_VALUE;

// Forward declarations
VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv);
VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode);
VOID LogEvent(const char* message); // Declare LogEvent before GetLocalIPv4Address uses it
VOID SvcInit(DWORD argc, LPTSTR* argv);
BOOL GetLocalIPv4Address(char* ipAddressBuffer, DWORD bufferSize); // Declare helper

// Helper function definition
BOOL GetLocalIPv4Address(char* ipAddressBuffer, DWORD bufferSize) {
    ULONG adaptersBufferSize = 15000; // Start with a reasonable buffer size
    PIP_ADAPTER_ADDRESSES pAddresses = NULL;
    PIP_ADAPTER_ADDRESSES pCurrAddresses = NULL;
    PIP_ADAPTER_UNICAST_ADDRESS pUnicast = NULL;
    DWORD dwRetVal = 0;
    BOOL found = FALSE;

    ipAddressBuffer[0] = '\0'; // Initialize buffer

    // Allocate buffer for adapter addresses
    pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(adaptersBufferSize);
    if (pAddresses == NULL) {
        LogEvent("GetLocalIPv4Address: Memory allocation failed for IP_ADAPTER_ADDRESSES struct.");
        return FALSE;
    }

    // Make an initial call to GetAdaptersAddresses to get the necessary size
    dwRetVal = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    NULL, pAddresses, &adaptersBufferSize);

    if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
        free(pAddresses);
        pAddresses = (IP_ADAPTER_ADDRESSES*)malloc(adaptersBufferSize);
        if (pAddresses == NULL) {
             LogEvent("GetLocalIPv4Address: Memory allocation failed (retry) for IP_ADAPTER_ADDRESSES struct.");
            return FALSE;
        }
        // Make the actual call to retrieve the data
        dwRetVal = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                        NULL, pAddresses, &adaptersBufferSize);
    }

    if (dwRetVal == NO_ERROR) {
        pCurrAddresses = pAddresses;
        while (pCurrAddresses) {
            // Check if adapter is up and not a loopback or tunnel interface
            if (pCurrAddresses->OperStatus == IfOperStatusUp && pCurrAddresses->IfType != IF_TYPE_SOFTWARE_LOOPBACK && pCurrAddresses->IfType != IF_TYPE_TUNNEL) {
                pUnicast = pCurrAddresses->FirstUnicastAddress;
                while (pUnicast) {
                    if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
                        struct sockaddr_in* sa_in = (struct sockaddr_in*)pUnicast->Address.lpSockaddr;
                        // Convert IP address to string
                        if (inet_ntop(AF_INET, &(sa_in->sin_addr), ipAddressBuffer, bufferSize) != NULL) {
                            char log_msg[128];
                            snprintf(log_msg, sizeof(log_msg), "GetLocalIPv4Address: Found suitable IP: %s on adapter %ws", ipAddressBuffer, pCurrAddresses->FriendlyName);
                            LogEvent(log_msg);
                            found = TRUE;
                            goto cleanup; // Found a suitable address
                        } else {
                             LogEvent("GetLocalIPv4Address: inet_ntop failed.");
                        }
                    }
                    pUnicast = pUnicast->Next;
                }
            }
            pCurrAddresses = pCurrAddresses->Next;
        }
    } else {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "GetLocalIPv4Address: GetAdaptersAddresses failed with error: %lu", dwRetVal);
        LogEvent(log_msg);
    }

cleanup:
    if (pAddresses) {
        free(pAddresses);
    }
    if (!found) {
         LogEvent("GetLocalIPv4Address: Could not find a suitable local IPv4 address.");
    }
    return found;
}

// LogEvent definition
VOID LogEvent(const char* message) {
    static char logFilePath[MAX_PATH] = {0};
    static BOOL path_initialized = FALSE;
    FILE* log = NULL;

    // Initialize log path on first call
    if (!path_initialized) {
        char exePath[MAX_PATH];
        char exeDir[MAX_PATH];

        // Get the full path of the current executable
        if (GetModuleFileNameA(NULL, exePath, MAX_PATH) == 0) {
            OutputDebugStringA("LogEvent: GetModuleFileNameA failed.\n");
            // Cannot proceed without the executable path
            return;
        }

        // Copy the path to manipulate it
        strcpy_s(exeDir, MAX_PATH, exePath);

        // Remove the file name part, leaving the directory
        if (!PathRemoveFileSpecA(exeDir)) {
             OutputDebugStringA("LogEvent: PathRemoveFileSpecA failed.\n");
             // Cannot proceed if directory extraction fails
             return;
        }

        // Combine the directory with the log file name
        if (!PathCombineA(logFilePath, exeDir, "upnps.log")) {
            OutputDebugStringA("LogEvent: PathCombineA failed.\n");
            // Cannot proceed if path construction fails
            return;
        }

        path_initialized = TRUE;
        // Update the global variable if it's used elsewhere (though its usage seems minimal now)
        strcpy_s(g_LogFilePath, MAX_PATH, logFilePath);
    }

    // If path initialization failed on the first attempt, logFilePath will be empty.
    if (logFilePath[0] == '\0') {
        OutputDebugStringA("LogEvent: Log path not initialized, cannot write log.\n");
        return;
    }

    // Open the log file for appending
    errno_t err = fopen_s(&log, logFilePath, "a");
    if (err == 0 && log != NULL) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(log, "%04d-%02d-%02d %02d:%02d:%02d.%03d: %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                message);
        fclose(log);
    } else {
        // Log failure to debug output if possible, as we can't write to the log file
        char errorMsg[MAX_PATH + 128];
        snprintf(errorMsg, sizeof(errorMsg), "LogEvent: Failed to open log file '%s'. fopen_s error: %d, errno: %d\n",
                 logFilePath, err, errno); // Include both fopen_s return and errno
        OutputDebugStringA(errorMsg);
    }
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR *argv) {
    LogEvent("ServiceMain entry.");

    g_StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);
    if (g_StatusHandle == NULL) {
        LogEvent("RegisterServiceCtrlHandler failed.");
        return;
    }

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwWin32ExitCode = NO_ERROR;

    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCheckPoint = 1;
    g_ServiceStatus.dwWaitHint = 10000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    LogEvent("Service START_PENDING (WaitHint=10s).");

    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (g_ServiceStopEvent == NULL) {
        LogEvent("CreateEvent failed for stop event.");
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    LogEvent("Service RUNNING.");

    SvcInit(argc, argv);

    LogEvent("SvcInit returned, service stopping.");

    g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCheckPoint = 1;
    g_ServiceStatus.dwWaitHint = 3000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    if (g_ServiceStopEvent) {
        CloseHandle(g_ServiceStopEvent);
        g_ServiceStopEvent = INVALID_HANDLE_VALUE;
        LogEvent("Stop event handle closed.");
    }

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwWin32ExitCode = NO_ERROR;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
    LogEvent("Service STOPPED.");
}

// Service control handler
VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    LogEvent("ServiceCtrlHandler entry.");
    switch (CtrlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            LogEvent("SERVICE_CONTROL_STOP or SHUTDOWN received.");
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            g_ServiceStatus.dwControlsAccepted = 0;
            g_ServiceStatus.dwCheckPoint = 1;
            g_ServiceStatus.dwWaitHint = 5000;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

            if (g_ServiceStopEvent != INVALID_HANDLE_VALUE) {
                SetEvent(g_ServiceStopEvent);
                LogEvent("Stop event signaled.");
            } else {
                LogEvent("Stop event handle is invalid in CtrlHandler!");
            }
            break;

        case SERVICE_CONTROL_INTERROGATE:
             SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
             break;

        default:
            break;
    }
}

// Main service logic
VOID SvcInit(DWORD argc, LPTSTR* argv) {
    LogEvent("SvcInit entry (main service logic starting).");

    // Initialize Winsock
    WSADATA wsaData;
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0) {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "SvcInit: WSAStartup failed with error: %d", wsaResult);
        LogEvent(log_msg);
        // Indicate failure to the SCM
        g_ServiceStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        g_ServiceStatus.dwServiceSpecificExitCode = wsaResult;
        return; // Cannot continue without Winsock
    } else {
        LogEvent("SvcInit: WSAStartup successful.");
    }

    const char * internal_port = "3306";
    const char * protocol = "TCP";
    int discovery_delay_ms = 10 * 60 * 1000; // 10 minutes in milliseconds
    const char * description = "UPnP Service Mapping";
    const char * remote_host = NULL;
    const char * lease_duration = "0";

    struct UPNPUrls urls;
    struct IGDdatas data;
    char lanaddr[64];
    char wanaddr[64];
    char external_ip[40];
    int upnp_error = 0;
    BOOL mapping_active = FALSE;

    char localIP[INET_ADDRSTRLEN] = {0}; // Buffer for local IP

    // Get local IP before starting the loop
    if (!GetLocalIPv4Address(localIP, sizeof(localIP))) {
        LogEvent("SvcInit: Failed to get local IP address. Discovery might fail or use default interface.");
        // Continue anyway, upnpDiscover might still work with NULL interface
    } else {
        char log_msg[INET_ADDRSTRLEN + 64];
        snprintf(log_msg, sizeof(log_msg), "SvcInit: Using local IP %s for multicast interface.", localIP);
        LogEvent(log_msg);
    }


    while (WaitForSingleObject(g_ServiceStopEvent, 0) != WAIT_OBJECT_0) {
        LogEvent("Worker loop iteration starting.");
        struct UPNPDev * devlist = NULL;
        int discover_error = 0;
        const char* multicast_if = (localIP[0] != '\0') ? localIP : NULL;

        char discover_log_msg[INET_ADDRSTRLEN + 128];
        snprintf(discover_log_msg, sizeof(discover_log_msg), "Discovering UPnP devices (5000ms timeout, source port 1900, interface: %s)...", multicast_if ? multicast_if : "default");
        LogEvent(discover_log_msg);

        // Use UPNP_LOCAL_PORT_ANY (0) instead of UPNP_LOCAL_PORT_SAME (1)
        devlist = upnpDiscover(5000, multicast_if, NULL, 0 /* UPNP_LOCAL_PORT_ANY */, 0, 2, &discover_error);

        if (devlist) {
            LogEvent("UPnP devices found. Getting valid IGD...");
            memset(&urls, 0, sizeof(urls));
            memset(&data, 0, sizeof(data));
            upnp_error = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));

            if (upnp_error > 0 && upnp_error <= 4) {
                char log_msg_igd[128];
                snprintf(log_msg_igd, sizeof(log_msg_igd), "Found IGD (lan %s, wan %s)", lanaddr, wanaddr);
                LogEvent(log_msg_igd);
                LogEvent("Attempting to add port mapping.");

                upnp_error = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external_ip);
                if (upnp_error == UPNPCOMMAND_SUCCESS) {
                    char log_msg[128];
                    snprintf(log_msg, sizeof(log_msg), "External IP: %s", external_ip);
                    LogEvent(log_msg);
                } else {
                    LogEvent("Failed to get external IP address.");
                }

                upnp_error = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                                 internal_port, internal_port, lanaddr,
                                                 description, protocol, remote_host, lease_duration);

                if (upnp_error == UPNPCOMMAND_SUCCESS) {
                    char log_msg[256];
                    snprintf(log_msg, sizeof(log_msg), "Successfully added mapping: %s %s -> %s:%s",
                             protocol, internal_port, lanaddr, internal_port);
                    LogEvent(log_msg);
                    mapping_active = TRUE;
                } else {
                    char log_msg[128];
                    snprintf(log_msg, sizeof(log_msg), "UPNP_AddPortMapping failed: %s (%d)",
                              strupnperror(upnp_error), upnp_error);
                     LogEvent(log_msg);
                     // If conflict (718), try deleting and adding again
                     if (upnp_error == 718) { // Use literal value 718
                         LogEvent("Conflict detected (718). Attempting to delete existing mapping...");
                         int delete_error = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype,
                                                                   internal_port, protocol, remote_host);
                         if (delete_error == UPNPCOMMAND_SUCCESS) {
                             LogEvent("Existing mapping deleted successfully. Retrying add...");
                         } else {
                             char delete_log_msg[128];
                             snprintf(delete_log_msg, sizeof(delete_log_msg), "UPNP_DeletePortMapping failed: %s (%d). Retrying add anyway...",
                                      strupnperror(delete_error), delete_error);
                             LogEvent(delete_log_msg);
                         }
                         // Retry adding the port mapping
                         upnp_error = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                                          internal_port, internal_port, lanaddr,
                                                          description, protocol, remote_host, lease_duration);
                         if (upnp_error == UPNPCOMMAND_SUCCESS) {
                             char retry_log_msg[256];
                             snprintf(retry_log_msg, sizeof(retry_log_msg), "Successfully added mapping on retry: %s %s -> %s:%s",
                                      protocol, internal_port, lanaddr, internal_port);
                             LogEvent(retry_log_msg);
                             mapping_active = TRUE;
                         } else {
                             char retry_fail_log_msg[128];
                             snprintf(retry_fail_log_msg, sizeof(retry_fail_log_msg), "UPNP_AddPortMapping failed on retry: %s (%d)",
                                      strupnperror(upnp_error), upnp_error);
                             LogEvent(retry_fail_log_msg);
                             mapping_active = FALSE;
                         }
                     } else {
                         // Original failure was not a conflict
                         mapping_active = FALSE;
                     }
                 }
                 FreeUPNPUrls(&urls);
            } else {
                char log_msg[128];
                snprintf(log_msg, sizeof(log_msg), "UPNP_GetValidIGD failed or no IGD found: code %d", upnp_error);
                LogEvent(log_msg);
                mapping_active = FALSE;
            }
            freeUPNPDevlist(devlist);
            devlist = NULL;
        } else {
            // Check the error code specifically
            if (discover_error == UPNPDISCOVER_SUCCESS) {
                LogEvent("upnpDiscover completed successfully, but no IGD devices were found.");
            } else {
                char log_msg[128];
                snprintf(log_msg, sizeof(log_msg), "upnpDiscover failed with error code: %d (%s)", discover_error, strupnperror(discover_error));
                LogEvent(log_msg);
            }
            mapping_active = FALSE; // No devices found or error occurred
        }

        LogEvent("Worker loop iteration finished. Waiting for next cycle or stop event.");

        DWORD waitResult = WaitForSingleObject(g_ServiceStopEvent, discovery_delay_ms);
        if (waitResult == WAIT_OBJECT_0) {
            LogEvent("Stop event detected during wait. Exiting loop.");
            break;
        } else if (waitResult != WAIT_TIMEOUT) {
            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg), "WaitForSingleObject in loop failed. Error code: %lu", GetLastError());
            LogEvent(log_msg);
            g_ServiceStatus.dwWin32ExitCode = GetLastError();
            break;
        }
    }

    LogEvent("SvcInit loop finished or stop event received. Cleaning up...");

    if (mapping_active) {
        LogEvent("Attempting to delete port mapping on exit...");
        struct UPNPDev * devlist_stop = NULL;
        int discover_error_stop = 0;
        devlist_stop = upnpDiscover(2000, NULL, NULL, 0, 0, 2, &discover_error_stop);
        if (devlist_stop) {
            struct UPNPUrls urls_stop;
            struct IGDdatas data_stop;
            char lanaddr_stop[64];
            char wanaddr_stop[64];
            memset(&urls_stop, 0, sizeof(urls_stop));
            memset(&data_stop, 0, sizeof(data_stop));
            int upnp_error_stop = UPNP_GetValidIGD(devlist_stop, &urls_stop, &data_stop, lanaddr_stop, sizeof(lanaddr_stop), wanaddr_stop, sizeof(wanaddr_stop));
            if (upnp_error_stop > 0 && upnp_error_stop <= 4) {
                LogEvent("Deleting port mapping...");
                upnp_error_stop = UPNP_DeletePortMapping(urls_stop.controlURL, data_stop.first.servicetype,
                                                         internal_port, protocol, remote_host);
                if (upnp_error_stop == UPNPCOMMAND_SUCCESS) {
                    LogEvent("Successfully deleted port mapping.");
                } else {
                     char log_msg[128];
                     snprintf(log_msg, sizeof(log_msg), "UPNP_DeletePortMapping failed: %s (%d)",
                              strupnperror(upnp_error_stop), upnp_error_stop);
                     LogEvent(log_msg);
                }
                FreeUPNPUrls(&urls_stop);
            } else {
                 char log_msg[128];
                 snprintf(log_msg, sizeof(log_msg), "Could not get valid IGD for cleanup: code %d", upnp_error_stop);
                 LogEvent(log_msg);
            }
            freeUPNPDevlist(devlist_stop);
        } else {
            LogEvent("Could not discover devices for cleanup.");
        }
    } else {
        LogEvent("Skipping port mapping deletion as no mapping was marked active.");
    }

    LogEvent("SvcInit exiting.");

    // Cleanup Winsock
    WSACleanup();
    LogEvent("SvcInit: WSACleanup called.");
}


int main(int argc, char *argv[]) {
    // Log very early, before service dispatcher potentially fails
    LogEvent("main entry - before StartServiceCtrlDispatcher.");

    SERVICE_TABLE_ENTRY ServiceTable[] = {
        {SERVICE_NAME, (LPSERVICE_MAIN_FUNCTION)ServiceMain},
        {NULL, NULL}
    };

    LogEvent("main: Calling StartServiceCtrlDispatcher...");
    if (StartServiceCtrlDispatcher(ServiceTable) == FALSE) {
        DWORD dwError = GetLastError();
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "main: StartServiceCtrlDispatcher failed. Error code: %lu", dwError);
        LogEvent(log_msg); // Log the failure

        // If running interactively, print help
        if (dwError == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
             LogEvent("Detected interactive run (ERROR_FAILED_SERVICE_CONTROLLER_CONNECT). Displaying help.");
             char exePath[MAX_PATH];
             DWORD pathLen = GetModuleFileNameA(NULL, exePath, MAX_PATH);

             printf("This program must be run as a Windows service.\n\n");
             printf("To install and manage the service, open an administrator command prompt and use the following sc commands:\n\n");

             if (pathLen > 0 && pathLen < MAX_PATH) {
                 char escapedPath[MAX_PATH * 2];
                 char *src = exePath;
                 char *dst = escapedPath;
                 while (*src) {
                     if (*src == '\\') {
                         *dst++ = '\\';
                     }
                     *dst++ = *src++;
                 }
                 *dst = '\0';
                 printf("Install: sc create %s binPath= \"%s\" start=auto\n", SERVICE_NAME, escapedPath);
             } else {
                 printf("Install: sc create %s binPath=\"<path\\to\\upnps.exe>\" start=auto (Error getting path)\n", SERVICE_NAME);
             }
             printf("Start:   sc start %s\n", SERVICE_NAME);
             printf("Stop:    sc stop %s\n", SERVICE_NAME);
             printf("Delete:  sc delete %s\n", SERVICE_NAME);
             fflush(stdout);
        } else {
             char error_msg_console[256];
             snprintf(error_msg_console, sizeof(error_msg_console), "Error: Could not connect to Service Control Manager. Error code: %lu\n", dwError);
             printf("%s", error_msg_console);
             fflush(stdout);
        }
        return dwError;
    }

    LogEvent("StartServiceCtrlDispatcher returned successfully. Service process exiting.");
    return 0;
}
