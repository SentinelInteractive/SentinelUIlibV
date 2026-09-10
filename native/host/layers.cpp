// SentinelUI ASI — layer stack implementation (issue #2).

#include "layers.h"
#include "ipc.h"
#include "suilog.h"

#include <algorithm>
#include <cwchar>

namespace sui_host {

LayerTable::Layer* LayerTable::FindLocked(int id) {
    for (auto& l : layers_)
        if (l->id == id) return l.get();
    return nullptr;
}

std::wstring LayerTable::ShmNameLocked(int id) const {
    if (id == kBaseLayerId) return shmBase_;
    return shmBase_ + L"_L" + std::to_wstring(id);
}

void LayerTable::CreateBase(const std::string& urlUtf8) {
    std::lock_guard<std::mutex> lock(mu_);
    if (FindLocked(kBaseLayerId)) return;
    auto l = std::make_unique<Layer>();
    l->id = kBaseLayerId;
    l->url = urlUtf8;
    l->z = 0;
    layers_.push_back(std::move(l));
    SUI_LOG_INFO("layer %d registered (boot): %s", kBaseLayerId, urlUtf8.c_str());
}

int LayerTable::Create(const std::string& urlUtf8) {
    std::lock_guard<std::mutex> lock(mu_);
    if ((int)layers_.size() >= kMaxLayers) {
        SUI_LOG_WARN("layer budget exhausted (%d); CreateLayer refused.", kMaxLayers);
        return 0;
    }
    int id = nextId_;
    while (FindLocked(id)) {  // wrap-safe scan for a free id
        if (++id > 999) id = 2;
        if (id == nextId_) break;
    }
    auto l = std::make_unique<Layer>();
    l->id = id;
    l->url = urlUtf8;
    // New layers stack above everything that already exists.
    int maxZ = 0;
    for (auto& existing : layers_) maxZ = (std::max)(maxZ, existing->z);
    l->z = maxZ + 1;
    layers_.push_back(std::move(l));
    nextId_ = id >= 999 ? 2 : id + 1;

    SendCommand("C" + std::to_string(id) + " " + std::to_string(urlUtf8.size()) + ":" +
                urlUtf8 + "\n");
    SUI_LOG_INFO("layer %d created: %s (z=%d)", id, urlUtf8.c_str(), maxZ + 1);
    return id;
}

void LayerTable::Close(int id) {
    std::unique_ptr<Layer> dead;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = layers_.begin(); it != layers_.end(); ++it) {
            if ((*it)->id == id) {
                dead = std::move(*it);
                layers_.erase(it);
                break;
            }
        }
    }
    if (!dead) return;
    if (dead->focused) {
        // Refocus the topmost remaining layer that can take input.
        int best = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& l : layers_) {
                if (l->visible && !l->clickThrough) {
                    if (!best || l->z > (FindLocked(best) ? FindLocked(best)->z : -1))
                        best = l->id;
                }
            }
        }
        if (best) SetFocus(best, true, true);
    }
    SendCommand("X" + std::to_string(id) + "\n");
    SUI_LOG_INFO("layer %d closed.", id);
}

void LayerTable::CloseAll() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& l : layers_) ids.push_back(l->id);
    }
    for (int id : ids) Close(id);
}

bool LayerTable::Exists(int id) {
    std::lock_guard<std::mutex> lock(mu_);
    return FindLocked(id) != nullptr;
}

bool LayerTable::Get(int id, Info& out) {
    std::lock_guard<std::mutex> lock(mu_);
    Layer* l = FindLocked(id);
    if (!l) return false;
    out.id = l->id;
    out.z = l->z;
    out.visible = l->visible;
    out.clickThrough = l->clickThrough;
    out.focused = l->focused;
    out.url = l->url;
    return true;
}

void LayerTable::SetVisible(int id, bool visible) {
    std::lock_guard<std::mutex> lock(mu_);
    Layer* l = FindLocked(id);
    if (!l || l->visible == visible) return;
    l->visible = visible;
    SendCommand(std::string(visible ? "S" : "H") + std::to_string(id) + "\n");
}

void LayerTable::SetZ(int id, int z) {
    std::lock_guard<std::mutex> lock(mu_);
    Layer* l = FindLocked(id);
    if (!l) return;
    l->z = z;
    SUI_LOG_INFO("layer %d z -> %d", id, z);
}

void LayerTable::SetClickThrough(int id, bool enabled) {
    std::lock_guard<std::mutex> lock(mu_);
    Layer* l = FindLocked(id);
    if (!l || l->clickThrough == enabled) return;
    l->clickThrough = enabled;
    if (l->focused && enabled) l->focused = false;  // click-through layers never hold focus
    SUI_LOG_INFO("layer %d click-through -> %s", id, enabled ? "on" : "off");
}

void LayerTable::SetFocus(int id, bool keyboard, bool mouse) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& l : layers_) l->focused = (l->id == id) && keyboard;
    Layer* target = id > 0 ? FindLocked(id) : nullptr;
    if (target && target->clickThrough) return;  // never focus a click-through layer
    SendCommand("F" + std::to_string(id) + (keyboard ? "1" : "0") + (mouse ? "1" : "0") + "\n");
}

std::vector<int> LayerTable::DrawOrder() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<int> order;
    for (auto& l : layers_)
        if (l->visible) order.push_back(l->id);
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        Layer* la = FindLocked(a);
        Layer* lb = FindLocked(b);
        int za = la ? la->z : 0;
        int zb = lb ? lb->z : 0;
        if (za != zb) return za < zb;
        return a < b;
    });
    return order;
}

int LayerTable::MouseTarget() {
    std::lock_guard<std::mutex> lock(mu_);
    int best = 0, bestZ = -0x7fffffff;
    for (auto& l : layers_) {
        if (!l->visible || l->clickThrough) continue;
        if (l->z > bestZ || (l->z == bestZ && best && l->id < best)) {
            best = l->id;
            bestZ = l->z;
        }
    }
    return best;
}

int LayerTable::KeyTarget() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& l : layers_)
        if (l->focused && l->visible) return l->id;
    // No explicit focus: input follows the topmost interactive layer.
    int best = 0, bestZ = -0x7fffffff;
    for (auto& l : layers_) {
        if (!l->visible || l->clickThrough) continue;
        if (l->z > bestZ) {
            best = l->id;
            bestZ = l->z;
        }
    }
    return best;
}

bool LayerTable::OpenFramesLocked(Layer& l) {
    if (l.framesOpen) return true;
    uint64_t now = GetTickCount64();
    if (now < l.nextOpenAttemptTick) return false;
    l.nextOpenAttemptTick = now + 250;  // retry at most 4x/s while the helper catches up
    if (!l.frames.Open(ShmNameLocked(l.id).c_str(), kMaxFrameW, kMaxFrameH)) return false;
    l.framesOpen = true;
    SUI_LOG_INFO("layer %d frames ready (%ls).", l.id, ShmNameLocked(l.id).c_str());
    return true;
}

std::vector<LayerTable::FrameInfo> LayerTable::PumpFrames(uint8_t* buf, size_t bufBytes, bool force) {
    std::vector<FrameInfo> fresh;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& l : layers_) {
        if (!l->visible) continue;
        if (!l->framesOpen && !OpenFramesLocked(*l)) continue;
        if (force) l->lastGen = 0;   // re-upload the resident latest frame
        uint64_t gen = l->lastGen;
        if (l->frames.Take(buf, bufBytes, gen)) {
            fresh.push_back({l->id, l->frames.width(), l->frames.height()});
            l->lastGen = gen;
        }
    }
    return fresh;
}

size_t LayerTable::Count() {
    std::lock_guard<std::mutex> lock(mu_);
    return layers_.size();
}

}  // namespace sui_host
