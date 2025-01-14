/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package io.trino.server;

import io.airlift.configuration.Config;

import java.io.File;

public class ServerPluginsProviderConfig
{
    private File installedPluginsDir = new File("plugin");

    private File trinoCppPluginsDir;

    public File getInstalledPluginsDir()
    {
        return installedPluginsDir;
    }

    public File getTrinoCppPluginsDir()
    {
        return trinoCppPluginsDir;
    }

    @Config("plugin.dir")
    public ServerPluginsProviderConfig setInstalledPluginsDir(File installedPluginsDir)
    {
        this.installedPluginsDir = installedPluginsDir;
        return this;
    }

    @Config("trino-cpp.plugin.dir")
    public ServerPluginsProviderConfig setTrinoCppPluginsDir(File trinoCppPluginsDir)
    {
        this.trinoCppPluginsDir = trinoCppPluginsDir;
        return this;
    }
}
