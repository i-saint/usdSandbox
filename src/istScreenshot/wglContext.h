#pragma once

#include <string>

class OffscreenWglContext
{
public:
    OffscreenWglContext() = default;
    ~OffscreenWglContext();

    OffscreenWglContext(const OffscreenWglContext&) = delete;
    OffscreenWglContext& operator=(const OffscreenWglContext&) = delete;

    bool Initialize(std::string* errorMessage = nullptr);
    void Shutdown();

    bool MakeCurrent() const;
    bool IsValid() const { return m_hglrc != nullptr; }

private:
    void* m_hwnd = nullptr;
    void* m_hdc = nullptr;
    void* m_hglrc = nullptr;
};
