#include "EntityProvider.h"

#include <QDir>
#include <functional>

#include "tasks/Task.h"
#include "Script.h"
#include "LuaUtil.h"
#include "BaseVersionList.h"
#include "BaseInstance.h"
#include "Json.h"
#include "ScriptEntityVersion.h"
#include "ScriptEntityVersionList.h"

QVector<EntityProvider::Entity> convert(const sol::table &in, EntityProvider *provider)
{

	QVector<EntityProvider::Entity> out;
	for (const auto &pair : in)
	{
		const sol::table table = LuaUtil::required<sol::table>(pair.second);
		out.append(EntityProvider::Entity{
									provider,
									LuaUtil::requiredString(table, "id"),
									LuaUtil::requiredString(table, "name"),
									LuaUtil::optionalString(table, "icon_url")
								});
	}
	return out;
}

EntityProvider::EntityProvider(const sol::table &table, Script *script)
	: QObject(script), m_script(script)
{
	m_id = LuaUtil::requiredString(table, "id");

	if (table["static_entities"])
	{
		m_staticEntities = convert(LuaUtil::required<sol::table>(table, "static_entities"), this);
	}

	auto internalUpdateFunc = LuaUtil::optional<sol::protected_function>(table, "dynamic_entities");
	m_entitiesUpdateFunc = [this, internalUpdateFunc](const sol::table &ctxt)
	{
		if (internalUpdateFunc)
		{
			return convert(internalUpdateFunc(ctxt), this);
		}
		return QVector<Entity>();
	};

	if (m_staticEntities.isEmpty() && !internalUpdateFunc)
	{
		throw ScriptLoadException(QString("Provided entity with ID '%1' does not provide any entities, either static_entities or dynamic_entities need to be non-empty").arg(m_id));
	}

	auto internalListFactoryFunc = LuaUtil::required<std::function< sol::table(std::string) >>(table, "version_list_factory");
	m_versionListFactoryFunc = [this, internalListFactoryFunc](const Entity &entity)
	{
		return std::make_shared<ScriptEntityVersionList>(internalListFactoryFunc(entity.internalId.toStdString()), entity, this);
	};
}

std::unique_ptr<Task> EntityProvider::createUpdateEntitiesTask()
{
	return std::make_unique<ScriptTask>([this](ScriptTask *task)
	{
		emit beforeEntitiesUpdate();
		m_entities = m_entitiesUpdateFunc(task->taskContext());
		emit afterEntitiesUpdate();
	}, m_script, this);
}
std::shared_ptr<BaseVersionList> EntityProvider::versionList(const EntityProvider::Entity &entity)
{
	if (!m_versionLists.contains(entity.internalId))
	{
		m_versionLists.insert(entity.internalId, m_versionListFactoryFunc(entity));
	}
	return m_versionLists.value(entity.internalId);
}

static void scriptWritePatch(BaseInstance *instance, const ScriptVersionPtr &version, const sol::table &data)
{
	const EntityProvider::Entity entity = version->versionList()->entity();

	QJsonObject obj = LuaUtil::toJsonObject(data);
	obj.insert("name", entity.name);
	obj.insert("fileId", entity.internalId);
	obj.insert("version", version->name());
	if (!data["mcVersion"] && version->table()["mcVersion"])
	{
		obj.insert("mcVersion", QString::fromStdString(version->table().get<std::string>("mcVersion")));
	}

	const QString filename = QDir(instance->instanceRoot()).absoluteFilePath("patches/" + version->versionList()->entity().internalId + ".json");
	Json::write(obj, filename);
}

std::unique_ptr<Task> EntityProvider::createInstallTask(BaseInstance *instance, const std::shared_ptr<BaseVersion> &version)
{
	const ScriptVersionPtr ver = std::dynamic_pointer_cast<ScriptEntityVersion>(version);
	if (!ver)
	{
		return nullptr;
	}
	return std::make_unique<ScriptTask>([this, instance, ver](ScriptTask *task)
	{
		// use the installer function in the version, or fallback to the one in the version list
		const sol::protected_function installerFunc = ver->table().get_or(
					"install",
					ver->versionList()->table().get<sol::protected_function>("install"));

		sol::table context = task->taskContext();
		context["write_patch"] = sol::as_function([this, instance, ver](const sol::table &data) { scriptWritePatch(instance, ver, data); });
		context["reload"] = sol::as_function([this, instance]() { instance->reload(); });

		sol::protected_function_result res = installerFunc(context, ver->table());
		if (!res)
		{
			sol::error err = res;
			throw Exception(QString("Unable to install %1 %2: %3").arg(ver->versionList()->entity().internalId, ver->name(), QString(err.what())));
		}
	}, m_script, this);
}
QVector<EntityProvider::Entity> EntityProvider::entities() const
{
	return m_entities + m_staticEntities;
}
