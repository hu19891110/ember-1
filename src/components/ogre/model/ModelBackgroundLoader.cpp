//
// C++ Implementation: ModelBackgroundLoader
//
// Description:
//
//
// Author: Erik Hjortsberg <erik.hjortsberg@gmail.com>, (C) 2009
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.//
//
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ModelBackgroundLoader.h"
#include "Model.h"
#include "framework/TimeFrame.h"
#include "framework/LoggingInstance.h"
#include "framework/Time.h"

#include <OgreResourceBackgroundQueue.h>
#include <OgreSubMesh.h>
#include <OgreMaterialManager.h>
#include <OgreMeshManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreTextureManager.h>

#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreTextureUnitState.h>

#include <Eris/EventService.h>

namespace Ember
{
namespace OgreView
{

namespace Model
{

ModelBackgroundLoaderListener::ModelBackgroundLoaderListener(ModelBackgroundLoader& loader) :
		mLoader(loader)
{

}
void ModelBackgroundLoaderListener::operationCompleted(Ogre::BackgroundProcessTicket ticket, const Ogre::BackgroundProcessResult& result)
{
	mLoader.operationCompleted(ticket, result, this);
}

ModelBackgroundLoader::ModelBackgroundLoader(Model* model, Eris::EventService& eventService) :
		mModel(model), mEventService(eventService), mState(LS_UNINITIALIZED), mListener(*this), mSubModelLoadingIndex(0)
{
}

ModelBackgroundLoader::~ModelBackgroundLoader()
{
	for (auto& ticket : mTickets) {
		Ogre::ResourceBackgroundQueue::getSingleton().abortRequest(ticket);
	}
}

bool ModelBackgroundLoader::poll()
{
	bool result = performLoading();
	if (result) {
		return true;
	} else {
		if (areAllTicketsProcessed()) {
			auto self = this->shared_from_this();
			mEventService.runOnMainThread([this, self]() {
				if (this->mModel) {
					bool result = this->poll();
					if (result) {
						this->reloadModel();
					}
				}
			});
		}
		return false;
	}
}

bool ModelBackgroundLoader::performLoading()
{
#if OGRE_THREAD_SUPPORT
	if (mState == LS_UNINITIALIZED) {
		//Start to load the meshes
		for (SubModelDefinitionsStore::const_iterator I_subModels = mModel->getDefinition()->getSubModelDefinitions().begin(); I_subModels != mModel->getDefinition()->getSubModelDefinitions().end(); ++I_subModels) {
			Ogre::MeshPtr meshPtr = static_cast<Ogre::MeshPtr>(Ogre::MeshManager::getSingleton().getByName((*I_subModels)->getMeshName()));
			if (meshPtr.isNull() || (!meshPtr->isPrepared() && !meshPtr->isLoading() && !meshPtr->isLoaded())) {
				try {
					Ogre::BackgroundProcessTicket ticket = Ogre::ResourceBackgroundQueue::getSingleton().prepare(Ogre::MeshManager::getSingleton().getResourceType(), (*I_subModels)->getMeshName(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, 0, 0, &mListener);
					if (ticket) {
						addTicket(ticket);
					}
				} catch (const std::exception& ex) {
					S_LOG_FAILURE("Could not load the mesh " << (*I_subModels)->getMeshName() << " when loading model " << mModel->getName() << "." << ex);
					continue;
				}
			}
		}
		mState = LS_MESH_PREPARING;
		return performLoading();
	} else if (mState == LS_MESH_PREPARING) {
		if (areAllTicketsProcessed()) {
			mState = LS_MESH_PREPARED;
			return performLoading();
		}
	} else if (mState == LS_MESH_PREPARED) {
		auto submodelDefinitions = mModel->getDefinition()->getSubModelDefinitions();
		while (mSubModelLoadingIndex < submodelDefinitions.size()) {
			auto submodelDef = submodelDefinitions.at(mSubModelLoadingIndex);
			mSubModelLoadingIndex++;
			Ogre::MeshPtr meshPtr = static_cast<Ogre::MeshPtr>(Ogre::MeshManager::getSingleton().getByName(submodelDef->getMeshName()));
			if (!meshPtr.isNull()) {
				if (!meshPtr->isLoaded()) {
#if OGRE_THREAD_SUPPORT == 1
					Ogre::BackgroundProcessTicket ticket = Ogre::ResourceBackgroundQueue::getSingleton().load(Ogre::MeshManager::getSingleton().getResourceType(), meshPtr->getName(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, 0, 0, mListener);
					if (ticket) {
						//						meshPtr->setBackgroundLoaded(true);
						addTicket(ticket);
					}
#else
					try {
						S_LOG_VERBOSE("Loading mesh in background loader: " << meshPtr->getName());
						meshPtr->load();
					} catch (const std::exception& ex) {
						S_LOG_FAILURE("Could not load the mesh " << meshPtr->getName() << " when loading model " << mModel->getName() << "." << ex);
					}
					return false;
#endif
				}
			}
		}
		mState = LS_MESH_LOADING;
		return performLoading();
	} else if (mState == LS_MESH_LOADING) {
		if (areAllTicketsProcessed()) {
			mState = LS_MESH_LOADED;
			return performLoading();
		}
	} else if (mState == LS_MESH_LOADED) {

		for (SubModelDefinitionsStore::const_iterator I_subModels = mModel->getDefinition()->getSubModelDefinitions().begin(); I_subModels != mModel->getDefinition()->getSubModelDefinitions().end(); ++I_subModels) {
			Ogre::MeshPtr meshPtr = static_cast<Ogre::MeshPtr>(Ogre::MeshManager::getSingleton().getByName((*I_subModels)->getMeshName()));
			if (!meshPtr.isNull()) {
				if (meshPtr->isLoaded()) {
					for (auto submesh : meshPtr->getSubMeshIterator()) {
						if (submesh->getMaterialName() != "") {
							Ogre::MaterialPtr materialPtr = static_cast<Ogre::MaterialPtr>(Ogre::MaterialManager::getSingleton().getByName(submesh->getMaterialName()));
							if (!materialPtr.isNull()) {
								mMaterialsToLoad.insert(materialPtr);
								if (!materialPtr->isPrepared() && !materialPtr->isLoading() && !materialPtr->isLoaded()) {
									Ogre::BackgroundProcessTicket ticket = Ogre::ResourceBackgroundQueue::getSingleton().prepare(Ogre::MaterialManager::getSingleton().getResourceType(), submesh->getMaterialName(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, 0, 0, &mListener);
									if (ticket) {
										addTicket(ticket);
									}
								}
							}
						}
					}
				}
			}
			for (PartDefinitionsStore::const_iterator I_parts = (*I_subModels)->getPartDefinitions().begin(); I_parts != (*I_subModels)->getPartDefinitions().end(); ++I_parts) {
				if ((*I_parts)->getSubEntityDefinitions().size() > 0) {
					for (SubEntityDefinitionsStore::const_iterator I_subEntities = (*I_parts)->getSubEntityDefinitions().begin(); I_subEntities != (*I_parts)->getSubEntityDefinitions().end(); ++I_subEntities) {
						const std::string& materialName = (*I_subEntities)->getMaterialName();
						if (materialName != "") {
							Ogre::MaterialPtr materialPtr = static_cast<Ogre::MaterialPtr>(Ogre::MaterialManager::getSingleton().getByName(materialName));
							if (!materialPtr.isNull()) {
								mMaterialsToLoad.insert(materialPtr);
								if (!materialPtr->isPrepared() && !materialPtr->isLoading() && !materialPtr->isLoaded()) {
									Ogre::BackgroundProcessTicket ticket = Ogre::ResourceBackgroundQueue::getSingleton().prepare(Ogre::MaterialManager::getSingleton().getResourceType(), materialName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, 0, 0, &mListener);
									if (ticket) {
										addTicket(ticket);
									}
								}
							}
						}
					}
				}
			}
		}

		mState = LS_MATERIAL_PREPARING;
		return performLoading();
	} else if (mState == LS_MATERIAL_PREPARING) {
		if (areAllTicketsProcessed()) {
			mState = LS_MATERIAL_PREPARED;
			return performLoading();
		}
	} else if (mState == LS_MATERIAL_PREPARED) {
		for (auto& materialPtr : mMaterialsToLoad) {
			if (!materialPtr.isNull()) {

#if OGRE_THREAD_SUPPORT == 1
				Ogre::BackgroundProcessTicket ticket = Ogre::ResourceBackgroundQueue::getSingleton().load(Ogre::MaterialManager::getSingleton().getResourceType(), materialPtr->getName(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, 0, 0, mListener);
				if (ticket) {
					//						materialPtr->setBackgroundLoaded(true);
					addTicket(ticket);
				}
#else
				Ogre::Material::TechniqueIterator techIter = materialPtr->getSupportedTechniqueIterator();
				while (techIter.hasMoreElements()) {
					Ogre::Technique* tech = techIter.getNext();
					Ogre::Technique::PassIterator passIter = tech->getPassIterator();
					while (passIter.hasMoreElements()) {
						Ogre::Pass* pass = passIter.getNext();
						Ogre::Pass::TextureUnitStateIterator tusIter = pass->getTextureUnitStateIterator();
						while (tusIter.hasMoreElements()) {
							Ogre::TextureUnitState* tus = tusIter.getNext();
							unsigned int frames = tus->getNumFrames();
							for (unsigned int i = 0; i < frames; ++i) {
								const auto& textureName = tus->getFrameTextureName(i);
								mTexturesToLoad.insert(textureName);
								Ogre::BackgroundProcessTicket ticket = Ogre::ResourceBackgroundQueue::getSingleton().prepare(Ogre::TextureManager::getSingleton().getResourceType(), textureName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, 0, 0, &mListener);
								if (ticket) {
									addTicket(ticket);
								}
							}
						}
					}
				}
#endif
			}
		}
		mState = LS_TEXTURE_PREPARING;
		return false;
	} else if (mState == LS_TEXTURE_PREPARING) {
		if (areAllTicketsProcessed()) {
			mState = LS_TEXTURE_PREPARED;
			return performLoading();
		}
	} else if (mState == LS_TEXTURE_PREPARED) {
		if (!mTexturesToLoad.empty()) {
			auto I = mTexturesToLoad.begin();
			auto textureName = *I;
			mTexturesToLoad.erase(I);
			auto texture = Ogre::TextureManager::getSingleton().getByName(textureName);
			if (!texture.isNull() && !texture->isLoaded()) {
				try {
					S_LOG_VERBOSE("Loading texture in background loader: " << texture->getName());
					texture->load();
				} catch (const std::exception& e) {
					S_LOG_WARNING("Error when loading texture " << texture->getName() << e);
				}
				return false;
			}
		}
		mState = LS_MATERIAL_LOADING;
		return performLoading();
	} else if (mState == LS_MATERIAL_LOADING) {
		if (areAllTicketsProcessed()) {
			if (!mMaterialsToLoad.empty()) {
				auto I = mMaterialsToLoad.begin();
				auto material = *I;
				mMaterialsToLoad.erase(I);
				if (!material->isLoaded()) {
					try {
						S_LOG_VERBOSE("Loading material in background loader: " << material->getName());
						material->load();
					} catch (const std::exception& e) {
						S_LOG_WARNING("Error when loading material " << material->getName() << e);
					}
					return false;
				}
			}

			mState = LS_DONE;
			return true;
		}
	} else {
		return true;
	}
	return false;
#else
	//If there's no threading support, just return here.
	return true;
#endif
}

void ModelBackgroundLoader::operationCompleted(Ogre::BackgroundProcessTicket ticket, const Ogre::BackgroundProcessResult& result, ModelBackgroundLoaderListener* listener)
{
	auto I = mTickets.find(ticket);
	if (I != mTickets.end()) {
		mTickets.erase(I);
		if (mTickets.empty()) {
			auto self = this->shared_from_this();
			mEventService.runOnMainThread([this, self]() {
				if (this->mModel) {
					bool result = this->poll();
					if (result) {
						this->reloadModel();
					}
				}
			});
		}
	}
}

bool ModelBackgroundLoader::areAllTicketsProcessed()
{
	return mTickets.empty();
}

ModelBackgroundLoader::LoadingState ModelBackgroundLoader::getState() const
{
	return mState;
}

void ModelBackgroundLoader::reloadModel()
{
	mModel->reload();
}

void ModelBackgroundLoader::detachFromModel()
{
	mModel = nullptr;
	for (auto& ticket : mTickets) {
		Ogre::ResourceBackgroundQueue::getSingleton().abortRequest(ticket);
	}
	mTickets.clear();
}

void ModelBackgroundLoader::addTicket(Ogre::BackgroundProcessTicket ticket)
{
	mTickets.insert(ticket);
}

}

}
}
