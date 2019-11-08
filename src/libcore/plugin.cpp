#include <mitsuba/core/plugin.h>
#include <mitsuba/core/util.h>
#include <mitsuba/core/logger.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/filesystem.h>
#include <mitsuba/core/fresolver.h>
#include <mutex>
#include <unordered_map>

#if !defined(__WINDOWS__)
# include <dlfcn.h>
#else
# include <windows.h>
#endif

NAMESPACE_BEGIN(mitsuba)

class Plugin {
public:
    Plugin(const fs::path &path) : m_path(path) {
        #if defined(__WINDOWS__)
            m_handle = LoadLibraryW(path.native().c_str());
            if (!m_handle)
                Throw("Error while loading plugin \"%s\": %s", path.string(),
                      util::last_error());
        #else
            m_handle = dlopen(path.native().c_str(), RTLD_LAZY | RTLD_LOCAL);
            if (!m_handle)
                Throw("Error while loading plugin \"%s\": %s", path.string(),
                      dlerror());
        #endif

        try {
            using StringFunc = const char *(*)();
            plugin_name  = ((StringFunc) symbol("plugin_name"))();
            plugin_descr = ((StringFunc) symbol("plugin_descr"))();
        } catch (...) {
            this->~Plugin();
            throw;
        }
    }

    ~Plugin() {
        #if defined(__WINDOWS__)
            FreeLibrary(m_handle);
        #else
            dlclose(m_handle);
        #endif
    }

    void *symbol(const std::string &name) const {
        #if defined(__WINDOWS__)
            void *ptr = GetProcAddress(m_handle, name.c_str());
            if (!ptr)
                Throw("Could not resolve symbol \"%s\" in \"%s\": %s", name,
                      m_path.string(), util::last_error());
        #else
            void *ptr = dlsym(m_handle, name.c_str());
            if (!ptr)
                Throw("Could not resolve symbol \"%s\" in \"%s\": %s", name,
                      m_path.string(), dlerror());
        #endif
        return ptr;
    }

    const char *plugin_name  = nullptr;
    const char *plugin_descr = nullptr;

private:
    #if defined(__WINDOWS__)
        HMODULE m_handle;
    #else
        void *m_handle;
    #endif
    fs::path m_path;
};

struct PluginManager::PluginManagerPrivate {
    std::unordered_map<std::string, Plugin *> m_plugins;
    std::mutex m_mutex;

    Plugin *plugin(const std::string &name) {
        std::lock_guard<std::mutex> guard(m_mutex);

        /* Plugin already loaded? */
        auto it = m_plugins.find(name);
        if (it != m_plugins.end())
            return it->second;

        /* Build the full plugin file name */
        fs::path filename = fs::path("plugins") / name;

        #if defined(__WINDOWS__)
            filename.replace_extension(".dll");
        #elif defined(__OSX__)
            filename.replace_extension(".dylib");
        #else
            filename.replace_extension(".so");
        #endif

        const FileResolver *resolver = Thread::thread()->file_resolver();
        fs::path resolved = resolver->resolve(filename);

        if (fs::exists(resolved)) {
            Log(Info, "Loading plugin \"%s\" ..", filename.string());
            Plugin *plugin = new Plugin(resolved);
            /* New classes must be registered within the class hierarchy */
            Class::static_initialization();
            ///Statistics::instance()->log_plugin(shortName, description()); XXX
            m_plugins[name] = plugin;
            return plugin;
        }

        /* Plugin not found! */
        Throw("Plugin \"%s\" not found!", name.c_str());
    }
};

ref<PluginManager> PluginManager::m_instance = new PluginManager();

PluginManager::PluginManager() : d(new PluginManagerPrivate()) { }

PluginManager::~PluginManager() {
    std::lock_guard<std::mutex> guard(d->m_mutex);
    for (auto &pair: d->m_plugins)
        delete pair.second;
}

ref<Object> PluginManager::create_object(const Properties &props, const Class *class_) {
    Assert(class_ != nullptr);
    if (class_->name() == "Scene")
       return class_->construct(props);

    const Plugin *plugin = d->plugin(props.plugin_name());
    const Class *plugin_class = Class::for_name(plugin->plugin_name, class_->variant());
    Assert(plugin_class != nullptr);
    ref<Object> object = plugin_class->construct(props);

    if (!object->class_()->derives_from(class_)) {
        const Class *oc = object->class_();
        if (oc->parent())
            oc = oc->parent();

        Throw("Type mismatch when loading plugin \"%s\": Expected "
              "an instance of type \"%s\", got an instance of type \"%s\"",
              props.plugin_name(), class_->name(), oc->name());
    }

   return object;
}

std::vector<std::string> PluginManager::loaded_plugins() const {
    std::vector<std::string> list;
    std::lock_guard<std::mutex> guard(d->m_mutex);
    for (auto const &pair: d->m_plugins)
        list.push_back(pair.first);
    return list;
}

void PluginManager::ensure_plugin_loaded(const std::string &name) {
    (void) d->plugin(name);
}

NAMESPACE_END(mitsuba)
