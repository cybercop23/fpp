/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2026 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the LGPL v2.1 as described in the
 * included LICENSE.LGPL file.
 */

#include "fpp-pch.h"

#include "fpp-json.h"
#include "log.h"

#include "AudioSourceRegistry.h"

AudioSourceRegistry AudioSourceRegistry::INSTANCE;

void AudioSourceRegistry::registerSource(const AudioSource& src) {
    if (src.id.empty() || src.nodeName.empty()) {
        LogWarn(VB_MEDIAOUT, "AudioSourceRegistry: ignoring source with empty id or nodeName (plugin: %s)\n",
                src.plugin.c_str());
        return;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    for (auto& s : m_sources) {
        if (s.id == src.id) {
            s = src;
            LogDebug(VB_MEDIAOUT, "AudioSourceRegistry: updated source '%s' (node: %s)\n",
                     src.id.c_str(), src.nodeName.c_str());
            return;
        }
    }
    m_sources.push_back(src);
    LogInfo(VB_MEDIAOUT, "AudioSourceRegistry: registered source '%s' (node: %s, plugin: %s)\n",
            src.id.c_str(), src.nodeName.c_str(), src.plugin.c_str());
}

void AudioSourceRegistry::unregisterSource(const std::string& id) {
    std::unique_lock<std::mutex> lock(m_mutex);
    for (auto it = m_sources.begin(); it != m_sources.end(); ++it) {
        if (it->id == id) {
            LogInfo(VB_MEDIAOUT, "AudioSourceRegistry: unregistered source '%s'\n", id.c_str());
            m_sources.erase(it);
            return;
        }
    }
}

void AudioSourceRegistry::unregisterPluginSources(const std::string& plugin) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_sources.erase(std::remove_if(m_sources.begin(), m_sources.end(),
                                   [&plugin](const AudioSource& s) { return s.plugin == plugin; }),
                    m_sources.end());
}

Json::Value AudioSourceRegistry::toJson() const {
    std::unique_lock<std::mutex> lock(m_mutex);
    Json::Value result;
    result["sources"] = Json::Value(Json::arrayValue);
    for (const auto& s : m_sources) {
        Json::Value j;
        j["id"] = s.id;
        j["name"] = s.name;
        j["nodeName"] = s.nodeName;
        j["plugin"] = s.plugin;
        j["channels"] = s.channels;
        j["sampleRate"] = s.sampleRate;
        result["sources"].append(j);
    }
    return result;
}
