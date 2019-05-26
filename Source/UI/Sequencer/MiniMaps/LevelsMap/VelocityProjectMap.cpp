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
#include "VelocityProjectMap.h"
#include "ProjectNode.h"
#include "MidiTrack.h"
#include "MidiSequence.h"
#include "Pattern.h"
#include "PianoSequence.h"
#include "PlayerThread.h"
#include "HybridRoll.h"
#include "Lasso.h"
#include "NoteComponent.h"
#include "AnnotationEvent.h"
#include "MidiTrack.h"
#include "ColourIDs.h"

#define VELOCITY_MAP_BULK_REPAINT_START \
    if (this->isEnabled()) { this->setVisible(false); }

#define VELOCITY_MAP_BULK_REPAINT_END \
    if (this->isEnabled()) { this->setVisible(true); }

//===----------------------------------------------------------------------===//
// Child level component
//===----------------------------------------------------------------------===//

class VelocityMapNoteComponent final : public Component
{
public:

    VelocityMapNoteComponent(const Note &note, const Clip &clip) :
        note(note),
        clip(clip)
    {
        this->updateColour();
        this->setInterceptsMouseClicks(true, false);
        this->setMouseClickGrabsKeyboardFocus(false);
        this->setPaintingIsUnclipped(true);
    }

    inline float getBeat() const noexcept
    {
        return this->note.getBeat() + this->clip.getBeat();
    }

    inline float getLength() const noexcept
    {
        return this->note.getLength();
    }

    inline float getVelocity() const noexcept
    {
        return this->note.getVelocity() * this->clip.getVelocity();
    }

    inline void updateColour()
    {
        const Colour baseColour(findDefaultColour(ColourIDs::Roll::noteFill));
        this->colour = this->note.getTrackColour().
            interpolatedWith(baseColour, this->isEditable ? .4f : .55f).
            withAlpha(this->isEditable ? 0.7f : .1f);
    }

    void setRealBounds(float x, int y, float w, int h) noexcept
    {
        this->dx = x - floorf(x);
        this->dw = ceilf(w) - w;
        this->setBounds(int(floorf(x)), y, int(ceilf(w)), h);
    }

    void setEditable(bool editable)
    {
        if (this->isEditable == editable)
        {
            return;
        }

        this->isEditable = editable;

        this->setEnabled(editable);
        this->updateColour();

        if (editable)
        {
            // toBack() and toFront() use indexOf(this) so calling them sucks
            this->toFront(false);
            this->setMouseCursor(MouseCursor::UpDownResizeCursor);
        }
    }

    //===------------------------------------------------------------------===//
    // Component
    //===------------------------------------------------------------------===//

    void paint(Graphics &g) noexcept override
    {
        g.setColour(this->colour);
        g.fillRect(this->dx, 0.f, float(this->getWidth()) - this->dw, float(this->getHeight()));
        g.fillRect(this->dx, 0.f, float(this->getWidth()) - this->dw, 2.f);
    }

    bool hitTest(int, int y) noexcept override
    {
        // can be dragged individually by header line
        return this->isEditable && y < 4;
    }

    void mouseDown(const MouseEvent &e) override
    {
        if (e.mods.isLeftButtonDown())
        {
            this->note.getSequence()->checkpoint();
            this->velocityAnchor = this->getVelocity();
        }
    }

    void mouseDrag(const MouseEvent &e) override
    {
        // FIXME: remove magic number (which is a scroller height)
        const auto newVelocity = jlimit(0.f, 1.f,
            this->velocityAnchor - float(e.getDistanceFromDragStartY()) / 128.f);

        static_cast<PianoSequence *>(this->note.getSequence())->
            change(this->note, this->note.withVelocity(newVelocity), true);
    }

private:

    VelocityProjectMap *getParentMap() const
    {
        jassert(this->getParentComponent());
        return static_cast<VelocityProjectMap *>(this->getParentComponent());
    }

    const Note &note;
    const Clip &clip;

    Colour colour;

    float dx = 0.f;
    float dw = 0.f;

    float velocityAnchor = 0.f;
    bool isEditable = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VelocityMapNoteComponent)
};

//===----------------------------------------------------------------------===//
// Dragging helper
//===----------------------------------------------------------------------===//

class VelocityLevelDraggingHelper final : public Component
{
public:

    VelocityLevelDraggingHelper(VelocityProjectMap &map) : map(map)
    {
        this->setWantsKeyboardFocus(false);
        this->setInterceptsMouseClicks(false, false);
    }

    void paint(Graphics &g) override
    {
        g.setColour(Colours::black);
        g.strokePath(this->path, PathStrokeType(.5f));

        g.setColour(Colours::white.withAlpha(0.55f));
        g.fillPath(this->path);
    }

    void setStartPosition(const Point<float> &mousePos)
    {
        this->startPosition = mousePos.toDouble() / this->getParentSize();
    }

    void setEndPosition(const Point<float> &mousePos)
    {
        this->endPosition = mousePos.toDouble() / this->getParentSize();
    }

    void updateBounds()
    {
        const Point<double> parentSize(this->getParentSize());
        const auto start = (this->startPosition * parentSize).toFloat();
        const auto end = (this->endPosition * parentSize).toFloat();
        const auto x1 = jmin(start.getX(), end.getX());
        const auto x2 = jmax(start.getX(), end.getX());
        const auto y1 = jmin(start.getY(), end.getY());
        const auto y2 = jmax(start.getY(), end.getY());
        const Point<float> startOffset(x1 - 2, y1 - 2);
        this->line = { start, end };

        this->path.clear();
        path.startNewSubPath(end - startOffset);
        path.lineTo(start - startOffset);
        static Array<float> dashes(8.f, 4.f);
        PathStrokeType(3.f).createDashedStroke(this->path, this->path,
            dashes.getRawDataPointer(), dashes.size());

        this->setBounds(int(x1) - 2, int(y1) - 2,
            int(x2 - x1) + 2 * 2, int(y2 - y1) + 2 * 2);
    }

private:

    VelocityProjectMap &map;

    Point<double> startPosition;
    Point<double> endPosition;

    Line<float> line;
    Path path;

    const Point<double> getParentSize() const
    {
        if (const auto *p = this->getParentComponent())
        {
            return{ double(p->getWidth()), double(p->getHeight()) };
        }

        return{ 1.0, 1.0 };
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VelocityLevelDraggingHelper);
};

//===----------------------------------------------------------------------===//
// The map itself
//===----------------------------------------------------------------------===//

VelocityProjectMap::VelocityProjectMap(ProjectNode &parentProject, HybridRoll &parentRoll) :
    project(parentProject),
    roll(parentRoll)
{
    this->setInterceptsMouseClicks(true, true);
    this->setPaintingIsUnclipped(true);
    this->reloadTrackMap();
    this->project.addListener(this);
    this->roll.getLassoSelection().addChangeListener(this);
}

VelocityProjectMap::~VelocityProjectMap()
{
    this->roll.getLassoSelection().removeChangeListener(this);
    this->project.removeListener(this);
}

//===----------------------------------------------------------------------===//
// Component
//===----------------------------------------------------------------------===//

void VelocityProjectMap::resized()
{
    VELOCITY_MAP_BULK_REPAINT_START

    for (const auto &c : this->patternMap)
    {
        const auto sequenceMap = c.second.get();
        for (const auto &e : *sequenceMap)
        {
            jassert(e.second.get());
            this->applyNoteBounds(e.second.get());
        }
    }

    if (this->dragHelper != nullptr)
    {
        this->dragHelper->updateBounds();
    }

    VELOCITY_MAP_BULK_REPAINT_END
}

void VelocityProjectMap::mouseDown(const MouseEvent &e)
{
    if (e.mods.isLeftButtonDown())
    {
        this->dragHelper.reset(new VelocityLevelDraggingHelper(*this));
        this->addAndMakeVisible(this->dragHelper.get());
        this->dragHelper->setStartPosition(e.position);
        this->dragHelper->setEndPosition(e.position);
    }
}

void VelocityProjectMap::mouseDrag(const MouseEvent &e)
{
    if (this->dragHelper != nullptr)
    {
        this->dragHelper->setEndPosition(e.position);
        this->dragHelper->updateBounds();
    }
}

void VelocityProjectMap::mouseUp(const MouseEvent &e)
{
    if (this->dragHelper != nullptr)
    {
        Array<Note> notes;
        this->dragHelper = nullptr;
    }
}

//===----------------------------------------------------------------------===//
// ProjectListener
//===----------------------------------------------------------------------===//

#define forEachSequenceMapOfGivenTrack(map, child, track) \
    for (const auto &child : map) \
        if (child.first.getPattern()->getTrack() == track)

void VelocityProjectMap::onChangeMidiEvent(const MidiEvent &e1, const MidiEvent &e2)
{
    if (e1.isTypeOf(MidiEvent::Type::Note))
    {
        const Note &note = static_cast<const Note &>(e1);
        const Note &newNote = static_cast<const Note &>(e2);
        const auto *track = newNote.getSequence()->getTrack();

        forEachSequenceMapOfGivenTrack(this->patternMap, c, track)
        {
            auto &sequenceMap = *c.second.get();
            if (const auto component = sequenceMap[note].release())
            {
                sequenceMap.erase(note);
                sequenceMap[newNote] = UniquePointer<VelocityMapNoteComponent>(component);
                this->triggerBatchRepaintFor(component);
            }
        }
    }
}

void VelocityProjectMap::onAddMidiEvent(const MidiEvent &event)
{
    if (event.isTypeOf(MidiEvent::Type::Note))
    {
        const Note &note = static_cast<const Note &>(event);
        const auto *track = note.getSequence()->getTrack();

        VELOCITY_MAP_BULK_REPAINT_START

        forEachSequenceMapOfGivenTrack(this->patternMap, c, track)
        {
            auto &componentsMap = *c.second.get();
            const int i = track->getPattern()->indexOfSorted(&c.first);
            jassert(i >= 0);

            const Clip *clip = track->getPattern()->getUnchecked(i);
            auto component = new VelocityMapNoteComponent(note, *clip);
            componentsMap[note] = UniquePointer<VelocityMapNoteComponent>(component);
            this->addAndMakeVisible(component);
            this->triggerBatchRepaintFor(component);
        }

        VELOCITY_MAP_BULK_REPAINT_END
    }
}

void VelocityProjectMap::onRemoveMidiEvent(const MidiEvent &event)
{
    if (event.isTypeOf(MidiEvent::Type::Note))
    {
        const Note &note = static_cast<const Note &>(event);
        const auto *track = note.getSequence()->getTrack();

        VELOCITY_MAP_BULK_REPAINT_START

        forEachSequenceMapOfGivenTrack(this->patternMap, c, track)
        {
            auto &sequenceMap = *c.second.get();
            if (sequenceMap.contains(note))
            {
                sequenceMap.erase(note);
            }
        }

        VELOCITY_MAP_BULK_REPAINT_END
    }
}

void VelocityProjectMap::onAddClip(const Clip &clip)
{
    const SequenceMap *referenceMap = nullptr;
    const auto *track = clip.getPattern()->getTrack();
    if (!dynamic_cast<const PianoSequence *>(track->getSequence())) { return; }

    forEachSequenceMapOfGivenTrack(this->patternMap, c, track)
    {
        // Found a sequence map for the same track
        referenceMap = c.second.get();
        break;
    }

    if (referenceMap == nullptr)
    {
        jassertfalse;
        return;
    }

    auto sequenceMap = new SequenceMap();
    this->patternMap[clip] = UniquePointer<SequenceMap>(sequenceMap);

    VELOCITY_MAP_BULK_REPAINT_START

    for (const auto &e : *referenceMap)
    {
        const auto &note = e.first;
        const auto noteComponent = new VelocityMapNoteComponent(note, clip);
        (*sequenceMap)[note] = UniquePointer<VelocityMapNoteComponent>(noteComponent);
        this->addAndMakeVisible(noteComponent);
        this->applyNoteBounds(noteComponent);
    }

    VELOCITY_MAP_BULK_REPAINT_END
}

void VelocityProjectMap::onChangeClip(const Clip &clip, const Clip &newClip)
{
    if (this->patternMap.contains(clip))
    {
        // Set new key for existing sequence map
        auto *sequenceMap = this->patternMap[clip].release();
        this->patternMap.erase(clip);
        this->patternMap[newClip] = UniquePointer<SequenceMap>(sequenceMap);

        // And update all components within it, as their beats should change
        for (const auto &e : *sequenceMap)
        {
            this->batchRepaintList.add(e.second.get());
        }

        this->triggerAsyncUpdate();
    }
}

void VelocityProjectMap::onRemoveClip(const Clip &clip)
{
    VELOCITY_MAP_BULK_REPAINT_START

    if (this->patternMap.contains(clip))
    {
        this->patternMap.erase(clip);
    }

    VELOCITY_MAP_BULK_REPAINT_END
}

void VelocityProjectMap::onChangeTrackProperties(MidiTrack *const track)
{
    if (!dynamic_cast<const PianoSequence *>(track->getSequence())) { return; }

    VELOCITY_MAP_BULK_REPAINT_START

    for (const auto &c : this->patternMap)
    {
        const auto &componentsMap = *c.second.get();
        for (const auto &e : componentsMap)
        {
            e.second->updateColour();
        }
    }

    VELOCITY_MAP_BULK_REPAINT_END

    this->repaint();
}

void VelocityProjectMap::onReloadProjectContent(const Array<MidiTrack *> &tracks)
{
    this->reloadTrackMap();
}

void VelocityProjectMap::onAddTrack(MidiTrack *const track)
{
    if (!dynamic_cast<const PianoSequence *>(track->getSequence())) { return; }

    VELOCITY_MAP_BULK_REPAINT_START
    this->loadTrack(track);
    VELOCITY_MAP_BULK_REPAINT_END
}

void VelocityProjectMap::onRemoveTrack(MidiTrack *const track)
{
    if (!dynamic_cast<const PianoSequence *>(track->getSequence())) { return; }

    for (int i = 0; i < track->getPattern()->size(); ++i)
    {
        const auto &clip = *track->getPattern()->getUnchecked(i);
        if (this->patternMap.contains(clip))
        {
            this->patternMap.erase(clip);
        }
    }
}

void VelocityProjectMap::onChangeProjectBeatRange(float firstBeat, float lastBeat)
{
    this->projectFirstBeat = firstBeat;
    this->projectLastBeat = lastBeat;

    if (this->rollFirstBeat > firstBeat ||
        this->rollLastBeat < lastBeat)
    {
        this->rollFirstBeat = firstBeat;
        this->rollLastBeat = lastBeat;
        //this->resized(); // seems to cause glitches sometimes?
    }
}

void VelocityProjectMap::onChangeViewBeatRange(float firstBeat, float lastBeat)
{
    this->rollFirstBeat = firstBeat;
    this->rollLastBeat = lastBeat;
    //this->resized(); // seems to cause glitches sometimes?
}

void VelocityProjectMap::onChangeViewEditableScope(MidiTrack *const, const Clip &clip, bool)
{
    if (this->activeClip == clip)
    {
        return;
    }

    this->activeClip = clip;

    VELOCITY_MAP_BULK_REPAINT_START

    for (const auto &c : this->patternMap)
    {
        const bool editable = this->activeClip == c.first;
        const auto &componentsMap = *c.second.get();
        for (const auto &e : componentsMap)
        {
            e.second->setEditable(editable);
        }
    }

    VELOCITY_MAP_BULK_REPAINT_END
}

void VelocityProjectMap::changeListenerCallback(ChangeBroadcaster *source)
{
    // for convenience, let's set selected items as editable

    jassert(dynamic_cast<Lasso *>(source));
    const auto *selection = static_cast<Lasso *>(source);

    const auto *activeMap = this->patternMap.at(this->activeClip).get();
    jassert(activeMap);

    VELOCITY_MAP_BULK_REPAINT_START

    if (selection->getNumSelected() == 0)
    {
        for (const auto &e : *activeMap)
        {
            e.second->setEditable(true);
        }
    }
    else
    {
        for (const auto &e : *activeMap)
        {
            e.second->setEditable(false);
        }

        for (const auto *e : *selection)
        {
            // assuming we've subscribed only on a piano roll's lasso changes
            const auto *nc = static_cast<const NoteComponent *>(e);
            activeMap->at(nc->getNote())->setEditable(true);
        }
    }

    VELOCITY_MAP_BULK_REPAINT_END
}

//===----------------------------------------------------------------------===//
// Private
//===----------------------------------------------------------------------===//

void VelocityProjectMap::reloadTrackMap()
{
    this->patternMap.clear();

    VELOCITY_MAP_BULK_REPAINT_START

    const auto &tracks = this->project.getTracks();
    for (const auto *track : tracks)
    {
        if (dynamic_cast<const PianoSequence *>(track->getSequence()))
        {
            this->loadTrack(track);
        }
    }

    VELOCITY_MAP_BULK_REPAINT_END
}

void VelocityProjectMap::loadTrack(const MidiTrack *const track)
{
    if (track->getPattern() == nullptr)
    {
        return;
    }

    for (int i = 0; i < track->getPattern()->size(); ++i)
    {
        const Clip *clip = track->getPattern()->getUnchecked(i);

        auto sequenceMap = new SequenceMap();
        this->patternMap[*clip] = UniquePointer<SequenceMap>(sequenceMap);

        for (int j = 0; j < track->getSequence()->size(); ++j)
        {
            const MidiEvent *event = track->getSequence()->getUnchecked(j);
            if (event->isTypeOf(MidiEvent::Type::Note))
            {
                const Note *note = static_cast<const Note *>(event);
                const auto noteComponent = new VelocityMapNoteComponent(*note, *clip);
                (*sequenceMap)[*note] = UniquePointer<VelocityMapNoteComponent>(noteComponent);
                this->addAndMakeVisible(noteComponent);
                this->applyNoteBounds(noteComponent);
            }
        }
    }
}

void VelocityProjectMap::applyNoteBounds(VelocityMapNoteComponent *nc)
{
    const float rollLengthInBeats = (this->rollLastBeat - this->rollFirstBeat);
    const float projectLengthInBeats = (this->projectLastBeat - this->projectFirstBeat);

    const float beat = (nc->getBeat() - this->rollFirstBeat);
    const float mapWidth = float(this->getWidth()) * (projectLengthInBeats / rollLengthInBeats);

    const float x = (mapWidth * (beat / projectLengthInBeats));
    const float w = (mapWidth * (nc->getLength() / projectLengthInBeats));

    // at least 4 pixels are visible for 0 volume events:
    const int h = jmax(4, int(this->getHeight() * nc->getVelocity()));
    nc->setRealBounds(x, this->getHeight() - h, jmax(1.f, w), h);
}

void VelocityProjectMap::triggerBatchRepaintFor(VelocityMapNoteComponent *target)
{
    this->batchRepaintList.add(target);
    this->triggerAsyncUpdate();
}

void VelocityProjectMap::handleAsyncUpdate()
{
    // batch repaint & resize stuff
    if (this->batchRepaintList.size() > 0)
    {
        VELOCITY_MAP_BULK_REPAINT_START

        for (int i = 0; i < this->batchRepaintList.size(); ++i)
        {
            // There are still many cases when a scheduled component is deleted at this time:
            if (Component *component = this->batchRepaintList.getUnchecked(i))
            {
                this->applyNoteBounds(static_cast<VelocityMapNoteComponent *>(component));
            }
        }

        VELOCITY_MAP_BULK_REPAINT_END

        this->batchRepaintList.clearQuick();
    }
}
