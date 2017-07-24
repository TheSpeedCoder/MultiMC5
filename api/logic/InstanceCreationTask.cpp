#include "InstanceCreationTask.h"
#include "BaseInstanceProvider.h"
#include "settings/INISettingsObject.h"
#include "FileSystem.h"

//FIXME: remove this
#include "minecraft/onesix/OneSixInstance.h"

InstanceCreationTask::InstanceCreationTask(SettingsObjectPtr settings, BaseInstanceProvider* target, BaseVersionPtr version,
	const QString& instName, const QString& instIcon, const QString& instGroup)
{
	m_globalSettings = settings;
	m_target = target;
	m_instName = instName;
	m_instIcon = instIcon;
	m_instGroup = instGroup;
	m_version = version;
}

void InstanceCreationTask::executeTask()
{
	setStatus(tr("Creating instance from version %1").arg(m_version->name()));
    /*
	auto minecraftVersion = std::dynamic_pointer_cast<MinecraftVersion>(m_version);
	if(!minecraftVersion)
	{
		emitFailed(tr("The supplied version is not a Minecraft version."));
		return ;
	}
	*/

	QString stagingPath = m_target->getStagedInstancePath();
	QDir rootDir(stagingPath);

	auto instanceSettings = std::make_shared<INISettingsObject>(FS::PathCombine(stagingPath, "instance.cfg"));
	instanceSettings->registerSetting("InstanceType", "Legacy");

	instanceSettings->set("InstanceType", "OneSix");
	auto inst = new OneSixInstance(m_globalSettings, instanceSettings, stagingPath);
	inst->setComponentVersion("net.minecraft", m_version->descriptor());
	inst->setName(m_instName);
	inst->setIconKey(m_instIcon);
	inst->init();
	m_target->commitStagedInstance(stagingPath, stagingPath, m_instName, m_instGroup);
	emitSucceeded();
}
