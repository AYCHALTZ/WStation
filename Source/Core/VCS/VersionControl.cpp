/*
    This file is part of Helio Workstation.

    Helio is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Helio is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Helio. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Common.h"
#include "App.h"
#include "VersionControl.h"
#include "VersionControlEditor.h"
#include "TrackedItem.h"
#include "MidiSequence.h"
#include "SerializationKeys.h"
#include "SerializationKeys.h"

using namespace VCS;

VersionControl::VersionControl(WeakReference<VCS::TrackedItemsSource> parent) :
    pack(new Pack()),
    head(pack, parent),
    stashes(new StashesRepository(pack)),
    rootRevision(new Revision(pack, TRANS("defaults::newproject::firstcommit")))
{
    MessageManagerLock lock;
    this->addChangeListener(&this->head);
    this->head.moveTo(this->rootRevision);
}

VersionControl::~VersionControl()
{
    MessageManagerLock lock;
    this->removeChangeListener(&this->head);
}

VersionControlEditor *VersionControl::createEditor()
{
    return new VersionControlEditor(*this);
}

static StringArray recursiveGetHashes(const Revision *revision)
{
    StringArray sum;
    for (auto *child : revision->getChildren())
    {
        sum.addArray(recursiveGetHashes(child));
    }

    const uint32 revisionSum(revision->calculateHash());
    sum.add(String(revisionSum));
    return sum;
}

String VersionControl::calculateHash() const
{
    StringArray ids(recursiveGetHashes(this->rootRevision));
    ids.sort(true); // sort it not to depend on child order
    return String(CompileTimeHash(ids.joinIntoString("").toUTF8()));
}

//===----------------------------------------------------------------------===//
// VCS
//===----------------------------------------------------------------------===//

void VersionControl::moveHead(const Revision::Ptr revision)
{
    if (! revision->isEmpty())
    {
        this->head.moveTo(revision);
        this->sendChangeMessage();
    }
}

void VersionControl::checkout(const Revision::Ptr revision)
{
    if (! revision->isEmpty())
    {
        this->head.moveTo(revision);
        this->head.checkout();
        this->sendChangeMessage();
    }
}

void VersionControl::cherryPick(const Revision::Ptr revision, const Array<Uuid> uuids)
{
    if (! revision->isEmpty())
    {
        auto headRevision(this->head.getHeadingRevision());
        this->head.moveTo(revision);
        this->head.cherryPick(uuids);
        this->head.moveTo(headRevision);
        this->sendChangeMessage();
    }
}

void VersionControl::appendSubtree(const VCS::Revision::Ptr subtree, const String &appendRevisionId)
{
    if (appendRevisionId.isEmpty())
    {
        // if appendRevisionId is empty - replace root?
        // todo make clear how to clone projects
        //Logger::writeToLog("Warning: replacing history remote tree");
        //this->rootRevision = subtree;
    }
    else
    {
        Revision::Ptr headRevision(this->getRevisionById(this->rootRevision, appendRevisionId));
        if (!headRevision->isEmpty())
        {
            headRevision->addChild(subtree);
            this->sendChangeMessage();
        }
    }
}

Revision::Ptr VersionControl::updateShallowRevisionData(const String &id, const ValueTree &data)
{
    // TODO
    return {};
}

void VersionControl::quickAmendItem(TrackedItem *targetItem)
{
    RevisionItem::Ptr revisionRecord(new RevisionItem(this->pack, RevisionItem::Added, targetItem));
    this->head.getHeadingRevision()->addItem(revisionRecord);
    this->head.moveTo(this->head.getHeadingRevision());
    this->head.getHeadingRevision()->flush();
    this->pack->flush();
    this->sendChangeMessage();
}

bool VersionControl::resetChanges(SparseSet<int> selectedItems)
{
    if (selectedItems.size() == 0) { return false; }

    Revision::Ptr allChanges(this->head.getDiff());
    Array<RevisionItem::Ptr> changesToReset;

    for (int i = 0; i < selectedItems.size(); ++i)
    {
        const int index = selectedItems[i];
        if (index >= allChanges->getItems().size()) { return false; }
        if (RevisionItem *item = allChanges->getItems()[index])
        {
            changesToReset.add(item);
        }
    }

    this->head.resetChanges(changesToReset);
    return true;
}

bool VersionControl::resetAllChanges()
{
    Revision::Ptr allChanges(this->head.getDiff());
    Array<RevisionItem::Ptr> changesToReset;

    for (auto *item : allChanges->getItems())
    {
        changesToReset.add(item);
    }
    
    this->head.resetChanges(changesToReset);
    return true;
}

bool VersionControl::commit(SparseSet<int> selectedItems, const String &message)
{
    if (selectedItems.size() == 0) { return false; }

    Revision::Ptr newRevision(new Revision(this->pack, message));
    Revision::Ptr allChanges(this->head.getDiff());

    for (int i = 0; i < selectedItems.size(); ++i)
    {
        const int index = selectedItems[i];
        if (index >= allChanges->getItems().size()) { return false; }
        if (RevisionItem *item = allChanges->getItems()[index])
        {
            newRevision->addItem(item);
        }
    }

    Revision::Ptr headingRevision(this->head.getHeadingRevision());
    if (headingRevision == nullptr) { return false; }

    headingRevision->addChild(newRevision);
    this->head.moveTo(newRevision);

    newRevision->flush();
    this->pack->flush();

    this->sendChangeMessage();
    return true;
}


//===----------------------------------------------------------------------===//
// Stashes
//===----------------------------------------------------------------------===//

bool VersionControl::stash(SparseSet<int> selectedItems,
    const String &message, bool shouldKeepChanges)
{
    if (selectedItems.size() == 0) { return false; }
    
    Revision::Ptr newRevision(new Revision(this->pack, message));
    Revision::Ptr allChanges(this->head.getDiff());
    
    for (int i = 0; i < selectedItems.size(); ++i)
    {
        const int index = selectedItems[i];
        if (index >= allChanges->getItems().size()) { return false; }
        newRevision->addItem(allChanges->getItems()[index]);
    }
    
    this->stashes->addStash(newRevision);

    if (! shouldKeepChanges)
    {
        this->resetChanges(selectedItems);
    }
    
    this->sendChangeMessage();
    return true;
}

bool VersionControl::applyStash(const Revision::Ptr stash, bool shouldKeepStash)
{
    if (! stash->isEmpty())
    {
        Revision::Ptr headRevision(this->head.getHeadingRevision());
        this->head.moveTo(stash);
        this->head.cherryPickAll();
        this->head.moveTo(headRevision);
        
        if (! shouldKeepStash)
        {
            this->stashes->removeStash(stash);
        }
        
        this->sendChangeMessage();
        return true;
    }
    
    return false;
}

bool VersionControl::applyStash(const String &stashId, bool shouldKeepStash)
{
    return this->applyStash(this->stashes->getUserStashWithName(stashId), shouldKeepStash);
}

bool VersionControl::hasQuickStash() const
{
    return (! this->stashes->hasQuickStash());
}

bool VersionControl::quickStashAll()
{
    if (this->hasQuickStash())
    { return false; }

    Revision::Ptr allChanges(this->head.getDiff());
    this->stashes->storeQuickStash(allChanges);
    this->resetAllChanges();

    this->sendChangeMessage();
    return true;
}

bool VersionControl::applyQuickStash()
{
    if (! this->hasQuickStash())
    { return false; }
    
    Head tempHead(this->head);
    tempHead.mergeStateWith(this->stashes->getQuickStash());
    tempHead.cherryPickAll();
    this->stashes->resetQuickStash();
    
    this->sendChangeMessage();
    return true;
}


//===----------------------------------------------------------------------===//
// Serializable
//===----------------------------------------------------------------------===//

ValueTree VersionControl::serialize() const
{
    ValueTree tree(Serialization::Core::versionControl);

    tree.setProperty(Serialization::VCS::headRevisionId, this->head.getHeadingRevision()->getUuid(), nullptr);
    
    tree.appendChild(this->rootRevision->serialize(), nullptr);
    tree.appendChild(this->stashes->serialize(), nullptr);
    tree.appendChild(this->pack->serialize(), nullptr);
    tree.appendChild(this->head.serialize(), nullptr);
    
    return tree;
}

void VersionControl::deserialize(const ValueTree &tree)
{
    this->reset();

    const auto root = tree.hasType(Serialization::Core::versionControl) ?
        tree : tree.getChildWithName(Serialization::Core::versionControl);

    if (!root.isValid()) { return; }

    const String headId = root.getProperty(Serialization::VCS::headRevisionId);
    Logger::writeToLog("Head ID is " + headId);

    this->rootRevision->deserialize(root);
    this->stashes->deserialize(root);
    this->pack->deserialize(root);

    {
        const double h1 = Time::getMillisecondCounterHiRes();
        this->head.deserialize(root);
        const double h2 = Time::getMillisecondCounterHiRes();
        Logger::writeToLog("Loading index done in " + String(h2 - h1) + "ms");
    }
    
    Revision::Ptr headRevision(this->getRevisionById(this->rootRevision, headId));

    // здесь мы раньше полностью десериализовали состояние хэда.
    // если дерево истории со временеи становится большим, moveTo со всеми мержами занимает кучу времени.
    // если работать в десятками тысяч событий, загрузка индекса длится ~2ms, а пересборка индекса - ~500ms
    // поэтому moveTo убираем, оставляем pointTo
    
    if (! headRevision->isEmpty())
    {
        //const double t1 = Time::getMillisecondCounterHiRes();

        this->head.pointTo(headRevision);
        //this->head.moveTo(headRevision);

        //const double t2 = Time::getMillisecondCounterHiRes();
        //Logger::writeToLog("Building index done in " + String(t2 - t1) + "ms");
    }
//#endif
}

void VersionControl::reset()
{
    this->rootRevision->reset();
    this->head.reset();
    this->stashes->reset();
    this->pack->reset();
}


//===----------------------------------------------------------------------===//
// ChangeListener
//===----------------------------------------------------------------------===//

void VersionControl::changeListenerCallback(ChangeBroadcaster* source)
{
    // Project changed
    this->getHead().setDiffOutdated(true);
}


//===----------------------------------------------------------------------===//
// Private
//===----------------------------------------------------------------------===//

Revision::Ptr VersionControl::getRevisionById(const Revision::Ptr startFrom, const String &id) const
{
    //Logger::writeToLog("getRevisionById, iterating " + startFrom.getUuid());

    if (startFrom->getUuid() == id)
    {
        return startFrom;
    }

    for (auto *child : startFrom->getChildren())
    {
        Revision::Ptr search(this->getRevisionById(child, id));
        if (! search->isEmpty())
        {
            //Logger::writeToLog("search ok, returning " + search.getUuid());
            return search;
        }
    }

    return { new Revision(this->pack) };
}
