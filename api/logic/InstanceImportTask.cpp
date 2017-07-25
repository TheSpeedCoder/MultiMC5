#include "InstanceImportTask.h"
#include "BaseInstance.h"
#include "BaseInstanceProvider.h"
#include "FileSystem.h"
#include "Env.h"
#include "MMCZip.h"
#include "NullInstance.h"
#include "settings/INISettingsObject.h"
#include "icons/IIconList.h"
#include <QtConcurrentRun>

// FIXME: this does not belong here, it's Minecraft/Flame specific
#include "minecraft/MinecraftInstance.h"
#include "minecraft/MinecraftProfile.h"
#include "minecraft/flame/FileResolvingTask.h"
#include "minecraft/flame/PackManifest.h"
#include "Json.h"

InstanceImportTask::InstanceImportTask(SettingsObjectPtr settings, const QUrl sourceUrl, BaseInstanceProvider * target,
	const QString &instName, const QString &instIcon, const QString &instGroup)
{
	m_globalSettings = settings;
	m_sourceUrl = sourceUrl;
	m_target = target;
	m_instName = instName;
	m_instIcon = instIcon;
	m_instGroup = instGroup;
}

void InstanceImportTask::executeTask()
{
	InstancePtr newInstance;

	if (m_sourceUrl.isLocalFile())
	{
		m_archivePath = m_sourceUrl.toLocalFile();
		extractAndTweak();
	}
	else
	{
		setStatus(tr("Downloading modpack:\n%1").arg(m_sourceUrl.toString()));
		m_downloadRequired = true;

		const QString path = m_sourceUrl.host() + '/' + m_sourceUrl.path();
		auto entry = ENV.metacache()->resolveEntry("general", path);
		entry->setStale(true);
		m_filesNetJob.reset(new NetJob(tr("Modpack download")));
		m_filesNetJob->addNetAction(Net::Download::makeCached(m_sourceUrl, entry));
		m_archivePath = entry->getFullPath();
		auto job = m_filesNetJob.get();
		connect(job, &NetJob::succeeded, this, &InstanceImportTask::downloadSucceeded);
		connect(job, &NetJob::progress, this, &InstanceImportTask::downloadProgressChanged);
		connect(job, &NetJob::failed, this, &InstanceImportTask::downloadFailed);
		m_filesNetJob->start();
	}
}

void InstanceImportTask::downloadSucceeded()
{
	extractAndTweak();
	m_filesNetJob.reset();
}

void InstanceImportTask::downloadFailed(QString reason)
{
	emitFailed(reason);
	m_filesNetJob.reset();
}

void InstanceImportTask::downloadProgressChanged(qint64 current, qint64 total)
{
	setProgress(current / 2, total);
}

static QFileInfo findRecursive(const QString &dir, const QString &name)
{
	for (const auto info : QDir(dir).entryInfoList(QDir::NoDotAndDotDot | QDir::Dirs | QDir::Files, QDir::DirsLast))
	{
		if (info.isFile() && info.fileName() == name)
		{
			return info;
		}
		else if (info.isDir())
		{
			const QFileInfo res = findRecursive(info.absoluteFilePath(), name);
			if (res.isFile() && res.exists())
			{
				return res;
			}
		}
	}
	return QFileInfo();
}

void InstanceImportTask::extractAndTweak()
{
	setStatus(tr("Extracting modpack"));
	m_stagingPath = m_target->getStagedInstancePath();
	QDir extractDir(m_stagingPath);
	qDebug() << "Attempting to create instance from" << m_archivePath;

	m_extractFuture = QtConcurrent::run(QThreadPool::globalInstance(), MMCZip::extractDir, m_archivePath, extractDir.absolutePath());
	connect(&m_extractFutureWatcher, &QFutureWatcher<QStringList>::finished, this, &InstanceImportTask::extractFinished);
	connect(&m_extractFutureWatcher, &QFutureWatcher<QStringList>::canceled, this, &InstanceImportTask::extractAborted);
	m_extractFutureWatcher.setFuture(m_extractFuture);
}

void InstanceImportTask::extractFinished()
{
	if (m_extractFuture.result().isEmpty())
	{
		m_target->destroyStagingPath(m_stagingPath);
		emitFailed(tr("Failed to extract modpack"));
		return;
	}
	QDir extractDir(m_stagingPath);

	qDebug() << "Fixing permissions for extracted pack files...";
	QDirIterator it(extractDir, QDirIterator::Subdirectories);
	while (it.hasNext())
	{
		auto filepath = it.next();
		QFileInfo file(filepath);
		auto permissions = QFile::permissions(filepath);
		auto origPermissions = permissions;
		if(file.isDir())
		{
			// Folder +rwx for current user
			permissions |= QFileDevice::Permission::ReadUser | QFileDevice::Permission::WriteUser | QFileDevice::Permission::ExeUser;
		}
		else
		{
			// File +rw for current user
			permissions |= QFileDevice::Permission::ReadUser | QFileDevice::Permission::WriteUser;
		}
		if(origPermissions != permissions)
		{
			if(!QFile::setPermissions(filepath, permissions))
			{
				qWarning() << "Could not fix" << filepath;
			}
			else
			{
				qDebug() << "Fixed" << filepath;
			}
		}
	}

	const QFileInfo instanceCfgFile = findRecursive(extractDir.absolutePath(), "instance.cfg");
	const QFileInfo flameJson = findRecursive(extractDir.absolutePath(), "manifest.json");
	if (instanceCfgFile.isFile())
	{
		qDebug() << "Pack appears to be exported from MultiMC.";
		processMultiMC(instanceCfgFile);
	}
	else if (flameJson.isFile())
	{
		qDebug() << "Pack appears to be from 'Flame'.";
		processFlame(flameJson);
	}
	else
	{
		qCritical() << "Archive does not contain a recognized modpack type.";
		m_target->destroyStagingPath(m_stagingPath);
		emitFailed(tr("Archive does not contain a recognized modpack type."));
	}
}

void InstanceImportTask::extractAborted()
{
	m_target->destroyStagingPath(m_stagingPath);
	emitFailed(tr("Instance import has been aborted."));
	return;
}

void InstanceImportTask::processFlame(const QFileInfo & manifest)
{
	const static QMap<QString,QString> forgemap = {
		{"1.2.5", "3.4.9.171"},
		{"1.4.2", "6.0.1.355"},
		{"1.4.7", "6.6.2.534"},
		{"1.5.2", "7.8.1.737"}
	};
	Flame::Manifest pack;
	try
	{
		Flame::loadManifest(pack, manifest.absoluteFilePath());
	}
	catch (JSONValidationError & e)
	{
		m_target->destroyStagingPath(m_stagingPath);
		emitFailed(tr("Could not understand pack manifest:\n") + e.cause());
		return;
	}
	m_packRoot = manifest.absolutePath();
	if(!pack.overrides.isEmpty())
	{
		QString overridePath = FS::PathCombine(m_packRoot, pack.overrides);
		QString mcPath = FS::PathCombine(m_packRoot, "minecraft");
		if (!QFile::rename(overridePath, mcPath))
		{
			m_target->destroyStagingPath(m_stagingPath);
			emitFailed(tr("Could not rename the overrides folder:\n") + pack.overrides);
			return;
		}
	}

	QString forgeVersion;
	for(auto &loader: pack.minecraft.modLoaders)
	{
		auto id = loader.id;
		if(id.startsWith("forge-"))
		{
			id.remove("forge-");
			forgeVersion = id;
			continue;
		}
		qWarning() << "Unknown mod loader in manifest:" << id;
	}

	QString configPath = FS::PathCombine(m_packRoot, "instance.cfg");
	auto instanceSettings = std::make_shared<INISettingsObject>(configPath);
	instanceSettings->registerSetting("InstanceType", "Legacy");
	instanceSettings->set("InstanceType", "OneSix");
	MinecraftInstance instance(m_globalSettings, instanceSettings, m_packRoot);
	auto mcVersion = pack.minecraft.version;
	// Hack to correct some 'special sauce'...
	if(mcVersion.endsWith('.'))
	{
		mcVersion.remove(QRegExp("[.]+$"));
		qWarning() << "Mysterious trailing dots removed from Minecraft version while importing pack.";
	}
	instance.setComponentVersion("net.minecraft", mcVersion);
	if(!forgeVersion.isEmpty())
	{
		// FIXME: dirty, nasty, hack. Proper solution requires dependency resolution and knowledge of the metadata.
		if(forgeVersion == "recommended")
		{
			if(forgemap.contains(mcVersion))
			{
				forgeVersion = forgemap[mcVersion];
			}
			else
			{
				qWarning() << "Could not map recommended forge version for" << mcVersion;
			}
		}
		instance.setComponentVersion("net.minecraftforge", forgeVersion);
	}
	if (m_instIcon != "default")
	{
		instance.setIconKey(m_instIcon);
	}
	else
	{
		if(pack.name.contains("Direwolf20"))
		{
			instance.setIconKey("steve");
		}
		else if(pack.name.contains("FTB") || pack.name.contains("Feed The Beast"))
		{
			instance.setIconKey("ftb_logo");
		}
		else
		{
			// default to something other than the MultiMC default to distinguish these
			instance.setIconKey("flame");
		}
	}
	instance.init();
	QString jarmodsPath = FS::PathCombine(m_packRoot, "minecraft", "jarmods");
	QFileInfo jarmodsInfo(jarmodsPath);
	if(jarmodsInfo.isDir())
	{
		// install all the jar mods
		qDebug() << "Found jarmods:";
		QDir jarmodsDir(jarmodsPath);
		QStringList jarMods;
		for (auto info: jarmodsDir.entryInfoList(QDir::NoDotAndDotDot | QDir::Files))
		{
			qDebug() << info.fileName();
			jarMods.push_back(info.absoluteFilePath());
		}
		auto profile = instance.getMinecraftProfile();
		profile->installJarMods(jarMods);
		// nuke the original files
		FS::deletePath(jarmodsPath);
	}
	instance.setName(m_instName);
	m_modIdResolver.reset(new Flame::FileResolvingTask(pack));
	connect(m_modIdResolver.get(), &Flame::FileResolvingTask::succeeded, [&]()
	{
		auto results = m_modIdResolver->getResults();
		m_filesNetJob.reset(new NetJob(tr("Mod download")));
		for(auto result: results.files)
		{
			auto path = FS::PathCombine(m_packRoot, "minecraft/mods", result.fileName);
			auto dl = Net::Download::makeFile(result.url,path);
			m_filesNetJob->addNetAction(dl);
		}
		m_modIdResolver.reset();
		connect(m_filesNetJob.get(), &NetJob::succeeded, this, [&]()
		{
			m_filesNetJob.reset();
			if (!m_target->commitStagedInstance(m_stagingPath, m_packRoot, m_instName, m_instGroup))
			{
				m_target->destroyStagingPath(m_stagingPath);
				emitFailed(tr("Unable to commit instance"));
				return;
			}
			emitSucceeded();
		}
		);
		connect(m_filesNetJob.get(), &NetJob::failed, [&](QString reason)
		{
			m_target->destroyStagingPath(m_stagingPath);
			m_filesNetJob.reset();
			emitFailed(reason);
		});
		connect(m_filesNetJob.get(), &NetJob::progress, [&](qint64 current, qint64 total)
		{
			setProgress(current, total);
		});
		setStatus(tr("Downloading mods..."));
		m_filesNetJob->start();
	}
	);
	connect(m_modIdResolver.get(), &Flame::FileResolvingTask::failed, [&](QString reason)
	{
		m_target->destroyStagingPath(m_stagingPath);
		m_modIdResolver.reset();
		emitFailed(tr("Unable to resolve mod IDs:\n") + reason);
	});
	connect(m_modIdResolver.get(), &Flame::FileResolvingTask::progress, [&](qint64 current, qint64 total)
	{
		setProgress(current, total);
	});
	connect(m_modIdResolver.get(), &Flame::FileResolvingTask::status, [&](QString status)
	{
		setStatus(status);
	});
	m_modIdResolver->start();
}

void InstanceImportTask::processMultiMC(const QFileInfo & config)
{
	// FIXME: copy from FolderInstanceProvider!!! FIX IT!!!
	auto instanceSettings = std::make_shared<INISettingsObject>(config.absoluteFilePath());
	instanceSettings->registerSetting("InstanceType", "Legacy");

	QString actualDir = config.absolutePath();
	NullInstance instance(m_globalSettings, instanceSettings, actualDir);

	// reset time played on import... because packs.
	instance.resetTimePlayed();

	// set a new nice name
	instance.setName(m_instName);

	// if the icon was specified by user, use that. otherwise pull icon from the pack
	if (m_instIcon != "default")
	{
		instance.setIconKey(m_instIcon);
	}
	else
	{
		m_instIcon = instance.iconKey();
		auto importIconPath = FS::PathCombine(instance.instanceRoot(), m_instIcon + ".png");
		if (QFile::exists(importIconPath))
		{
			// import icon
			auto iconList = ENV.icons();
			if (iconList->iconFileExists(m_instIcon))
			{
				iconList->deleteIcon(m_instIcon);
			}
			iconList->installIcons({importIconPath});
		}
	}
	if (!m_target->commitStagedInstance(m_stagingPath, actualDir, m_instName, m_instGroup))
	{
		m_target->destroyStagingPath(m_stagingPath);
		emitFailed(tr("Unable to commit instance"));
		return;
	}
	emitSucceeded();
}
