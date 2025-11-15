#ifndef MOCK_WEB_SERVER_H
#define MOCK_WEB_SERVER_H

#include <Arduino.h>
#include <functional>
#include <map>
#include <vector>

// Mock WebServer for testing
class WebServer {
private:
    int port;
    bool running;
    std::map<String, std::function<void()>> handlers;
    std::function<void()> notFoundHandler;
    
    // Mock request/response state
    String lastResponseBody;
    String lastResponseType;
    int lastResponseCode;
    std::map<String, String> responseHeaders;

public:
    WebServer(int p) : port(p), running(false), lastResponseCode(0) {}
    
    void on(const String& uri, std::function<void()> handler) {
        handlers[uri] = handler;
    }
    
    void onNotFound(std::function<void()> handler) {
        notFoundHandler = handler;
    }
    
    void begin() {
        running = true;
    }
    
    void stop() {
        running = false;
    }
    
    void handleClient() {
        // Mock implementation - does nothing in tests
    }
    
    void send(int code, const String& contentType, const String& content) {
        lastResponseCode = code;
        lastResponseType = contentType;
        lastResponseBody = content;
    }
    
    void sendHeader(const String& name, const String& value) {
        responseHeaders[name] = value;
    }
    
    // Test helper methods
    bool isRunning() const { return running; }
    int getPort() const { return port; }
    bool hasHandler(const String& uri) const {
        return handlers.find(uri) != handlers.end();
    }
    
    void simulateRequest(const String& uri) {
        auto it = handlers.find(uri);
        if (it != handlers.end()) {
            it->second();
        } else if (notFoundHandler) {
            notFoundHandler();
        }
    }
    
    int getLastResponseCode() const { return lastResponseCode; }
    String getLastResponseType() const { return lastResponseType; }
    String getLastResponseBody() const { return lastResponseBody; }
    String getResponseHeader(const String& name) const {
        auto it = responseHeaders.find(name);
        return (it != responseHeaders.end()) ? it->second : "";
    }
    
    void clearResponse() {
        lastResponseCode = 0;
        lastResponseType = "";
        lastResponseBody = "";
        responseHeaders.clear();
    }
};

#endif // MOCK_WEB_SERVER_H
