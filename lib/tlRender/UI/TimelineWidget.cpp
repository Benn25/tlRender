// SPDX-License-Identifier: BSD-3-Clause
// Copyright Contributors to the tlRender project.

#include <tlRender/UI/TimelineWidget.h>

#include <tlRender/UI/TimelineRuler.h>

#include <tlRender/Core/Time.h>

#include <tlRender/Timeline/TimeUnits.h>

#include <ftk/UI/RowLayout.h>
#include <ftk/UI/ScrollWidget.h>

#include <algorithm>

namespace tl
{
    namespace ui
    {
        namespace
        {
            const float marginPercentage = .1F;
        }

        struct TimelineWidget::Private
        {
            std::shared_ptr<ITimeUnitsModel> timeUnitsModel;

            //! One per item: the media time conversion is bound to a
            //! particular timeline, so it cannot be shared between them.
            std::vector<std::shared_ptr<ItemData> > itemData;

            std::shared_ptr<Player> player;

            //! The timelines drawn beside the player's own, and where they
            //! came from: what the player is comparing, or what the embedder
            //! asked for.
            std::vector<std::shared_ptr<Timeline> > compare;
            std::optional<std::vector<std::shared_ptr<Timeline> > > timelines;
            std::shared_ptr<ftk::Observable<bool> > frameView;
            std::shared_ptr<ftk::Observable<bool> > scrollBarsVisible;
            std::shared_ptr<ftk::Observable<bool> > autoScroll;
            std::pair<ftk::MouseButton, ftk::KeyModifier> scrollBinding =
                std::make_pair(ftk::MouseButton::Middle, ftk::KeyModifier::None);
            float mouseWheelScale = 1.1F;
            std::shared_ptr<ftk::Observable<bool> > stopOnScrub;
            std::shared_ptr<ftk::Observable<bool> > scrub;
            std::shared_ptr<ftk::Observable<std::optional<OTIO_NS::RationalTime> > > timeScrub;
            std::shared_ptr<ftk::Observable<std::optional<OTIO_NS::RationalTime> > > timeHover;
            std::vector<int> frameMarkers;
            std::vector<TimeMark> timeMarks;
            std::vector<OTIO_NS::TimeRange> holes;
            std::vector<Marker> markers;
            std::vector<ItemColors> itemColors;
            std::vector<std::string> labels;
            std::vector<OTIO_NS::RationalTime> offsets;
            std::shared_ptr<ftk::Observable<ItemOptions> > itemOptions;
            std::shared_ptr<ftk::Observable<DisplayOptions> > displayOptions;
            OTIO_NS::TimeRange timeRange;
            Playback playback = Playback::Stop;
            OTIO_NS::RationalTime currentTime;
            double scale = 500.0;
            bool sizeInit = true;
            std::optional<double> zoomPending;

            //! The width the timelines were last framed against. A scroll bar
            //! appearing takes width from them without the widget's own size
            //! changing, so this is what says the framing is stale, and the
            //! widget's geometry is not.
            int frameViewWidth = -1;
            float displayScale = 0.F;
            int border = 0;

            //! One scroll area for every timeline, so that they share a scroll
            //! position rather than being kept in step with each other. The
            //! first item is the player's; the rest are what it is being
            //! compared against.
            std::shared_ptr<TimelineRuler> ruler;
            std::shared_ptr<ftk::ScrollWidget> scrollWidget;
            ftk::V2F gesturePan;
            std::shared_ptr<ftk::VerticalLayout> layout;
            std::vector<std::shared_ptr<TimelineItem> > timelineItems;

            enum class MouseMode
            {
                None,
                Scroll,
                InOutSet,
                InOutStart,
                InOutEnd
            };
            struct MouseData
            {
                bool inside = false;
                ftk::V2I press;
                MouseMode mode = MouseMode::None;
                ftk::V2I scrollPos;
                OTIO_NS::RationalTime inOutAnchor;
                bool inOutDragged = false;
            };
            MouseData mouse;

            std::shared_ptr<ftk::Observer<Playback> > playbackObserver;
            std::shared_ptr<ftk::Observer<OTIO_NS::RationalTime> > currentTimeObserver;
            OTIO_NS::TimeRange inOutRange;
            std::shared_ptr<ftk::Observer<OTIO_NS::TimeRange> > inOutRangeObserver;
            std::shared_ptr<ftk::Observer<std::string> > mediaReferenceKeyObserver;
            std::shared_ptr<ftk::ListObserver<std::shared_ptr<Timeline> > > compareObserver;
            std::shared_ptr<ftk::Observer<bool> > scrubObserver;
            std::shared_ptr<ftk::Observer<std::optional<OTIO_NS::RationalTime> > > timeScrubObserver;
            std::shared_ptr<ftk::Observer<std::optional<OTIO_NS::RationalTime> > > timeHoverObserver;
            std::shared_ptr<ftk::Observer<bool> > rulerScrubObserver;
            std::shared_ptr<ftk::Observer<std::optional<OTIO_NS::RationalTime> > > rulerTimeScrubObserver;
            std::shared_ptr<ftk::Observer<std::optional<OTIO_NS::RationalTime> > > rulerTimeHoverObserver;
        };

        void TimelineWidget::_init(
            const std::shared_ptr<ftk::Context>& context,
            const std::shared_ptr<ITimeUnitsModel>& timeUnitsModel,
            const std::shared_ptr<IWidget>& parent)
        {
            IWidget::_init(context, "tl::ui::TimelineWidget", parent);
            FTK_P();

            p.timeUnitsModel = timeUnitsModel ?
                timeUnitsModel :
                TimeUnitsModel::create(context);

            p.frameView = ftk::Observable<bool>::create(true);
            p.scrollBarsVisible = ftk::Observable<bool>::create(true);
            p.autoScroll = ftk::Observable<bool>::create(true);
            p.stopOnScrub = ftk::Observable<bool>::create(true);
            p.scrub = ftk::Observable<bool>::create(false);
            p.timeScrub = ftk::Observable<std::optional<OTIO_NS::RationalTime> >::create();
            p.timeHover = ftk::Observable<std::optional<OTIO_NS::RationalTime> >::create();
            p.itemOptions = ftk::Observable<ItemOptions>::create();
            p.displayOptions = ftk::Observable<DisplayOptions>::create();

            // The ruler is outside the scroll area so that it stays put while
            // the timelines scroll under it, and follows their scale and
            // scroll position instead. Laid out by hand rather than stacked
            // in a layout: it has to end where the scrolled timelines end,
            // and a vertical scroll bar takes width from them and not from a
            // sibling above them.
            p.ruler = TimelineRuler::create(context, nullptr, shared_from_this());

            p.scrollWidget = ftk::ScrollWidget::create(
                context,
                ftk::ScrollType::Both,
                shared_from_this());
            p.scrollWidget->setScrollBarsVisible(p.scrollBarsVisible->get());
            p.scrollWidget->setScrollEventsEnabled(false);
            p.scrollWidget->setBorder(false);

            p.ruler->setStopOnScrub(p.stopOnScrub->get());
            p.ruler->setOptions(p.itemOptions->get());

            p.rulerScrubObserver = ftk::Observer<bool>::create(
                p.ruler->observeScrub(),
                [this](bool value)
                {
                    _p->scrub->setIfChanged(value);
                    _scrollUpdate();
                });

            p.rulerTimeScrubObserver = ftk::Observer<std::optional<OTIO_NS::RationalTime> >::create(
                p.ruler->observeTimeScrub(),
                [this](const std::optional<OTIO_NS::RationalTime>& value)
                {
                    _p->timeScrub->setIfChanged(value);
                });

            p.rulerTimeHoverObserver = ftk::Observer<std::optional<OTIO_NS::RationalTime> >::create(
                p.ruler->observeTimeHover(),
                [this](const std::optional<OTIO_NS::RationalTime>& value)
                {
                    _p->timeHover->setIfChanged(value);
                });

            p.layout = ftk::VerticalLayout::create(context);
            p.layout->setSpacingRole(ftk::SizeRole::None);
            p.scrollWidget->setWidget(p.layout);

            p.scrollWidget->setScrollPosCallback(
                [this](const ftk::V2I& value)
                {
                    _p->ruler->setScrollPos(value.x);
                });
        }

        TimelineWidget::TimelineWidget() :
            _p(new Private)
        {}

        TimelineWidget::~TimelineWidget()
        {}

        std::shared_ptr<TimelineWidget> TimelineWidget::create(
            const std::shared_ptr<ftk::Context>& context,
            const std::shared_ptr<IWidget>& parent)
        {
            auto out = std::shared_ptr<TimelineWidget>(new TimelineWidget);
            out->_init(context, nullptr, parent);
            return out;
        }

        std::shared_ptr<TimelineWidget> TimelineWidget::create(
            const std::shared_ptr<ftk::Context>& context,
            const std::shared_ptr<ITimeUnitsModel>& timeUnitsModel,
            const std::shared_ptr<IWidget>& parent)
        {
            auto out = std::shared_ptr<TimelineWidget>(new TimelineWidget);
            out->_init(context, timeUnitsModel, parent);
            return out;
        }

        const std::shared_ptr<ITimeUnitsModel>& TimelineWidget::getTimeUnitsModel() const
        {
            return _p->timeUnitsModel;
        }

        std::shared_ptr<Player>& TimelineWidget::getPlayer() const
        {
            return _p->player;
        }

        void TimelineWidget::setPlayer(const std::shared_ptr<Player>& player)
        {
            FTK_P();
            if (player == p.player)
                return;

            // The same sequence reloaded -- grown, its frame range with it --
            // keeps its mark.
            const auto pathKey = [](const std::shared_ptr<Player>& value)
            {
                const ftk::Path& path = value->getPath();
                return path.getDir() + path.getBase() + path.getExt();
            };
            if (!player || !p.player || pathKey(player) != pathKey(p.player))
            {
                p.timeMarks.clear();
                p.holes.clear();
            }
            p.timeRange = OTIO_NS::TimeRange();
            p.playback = Playback::Stop;
            p.playbackObserver.reset();
            p.currentTimeObserver.reset();
            p.inOutRangeObserver.reset();
            p.mediaReferenceKeyObserver.reset();
            p.compareObserver.reset();

            p.player = player;
            p.compare = _getCompare();
            p.scale = _getTimelineScale();

            _timelineUpdate();

            if (p.player)
            {
                p.timeRange = p.player->getTimeRange();

                p.inOutRangeObserver = ftk::Observer<OTIO_NS::TimeRange>::create(
                    p.player->observeInOutRange(),
                    [this](const OTIO_NS::TimeRange& value)
                    {
                        _p->inOutRange = value;
                        setDrawUpdate();
                    });

                // The compare timelines are drawn beside the player's own, so
                // the items are rebuilt when the set changes.
                p.compareObserver = ftk::ListObserver<std::shared_ptr<Timeline> >::create(
                    p.player->observeCompare(),
                    [this](const std::vector<std::shared_ptr<Timeline> >&)
                    {
                        // Only when the drawn timelines are the player's; an
                        // embedder that says which to draw is not overruled by
                        // the player changing what it compares.
                        FTK_P();
                        if (!p.timelines.has_value())
                        {
                            p.compare = _getCompare();
                            _timelineUpdate();
                        }
                    },
                    ftk::ObserverAction::Suppress);

                p.playbackObserver = ftk::Observer<Playback>::create(
                    p.player->observePlayback(),
                    [this](Playback value)
                    {
                        _p->playback = value;
                    });

                // The clip items take their media from the timeline when they
                // are created, so they are rebuilt to follow the new media
                // reference. The items have just been built above, so the
                // current key is suppressed.
                p.mediaReferenceKeyObserver = ftk::Observer<std::string>::create(
                    p.player->observeMediaReferenceKey(),
                    [this](const std::string&)
                    {
                        _timelineUpdate();
                    },
                    ftk::ObserverAction::Suppress);

                p.currentTimeObserver = ftk::Observer<OTIO_NS::RationalTime>::create(
                    p.player->observeCurrentTime(),
                    [this](const OTIO_NS::RationalTime& value)
                    {
                        _p->currentTime = value;
                        _p->ruler->setScrollPos(
                            _p->scrollWidget->getScrollPos().x);
                        setDrawUpdate();
                        _scrollUpdate();
                    });
            }
        }

        double TimelineWidget::getViewZoom() const
        {
            return _p->scale;
        }

        void TimelineWidget::setTimelines(
            const std::optional<std::vector<std::shared_ptr<Timeline> > >& value)
        {
            FTK_P();
            if (value == p.timelines)
                return;
            p.timelines = value;
            p.compare = _getCompare();
            _timelineUpdate();
            setSizeUpdate();
            setDrawUpdate();
        }

        std::vector<std::shared_ptr<Timeline> > TimelineWidget::_getCompare() const
        {
            FTK_P();
            if (p.timelines.has_value())
                return p.timelines.value();
            return p.player ?
                p.player->getCompare() :
                std::vector<std::shared_ptr<Timeline> >();
        }

        void TimelineWidget::setViewZoom(double value)
        {
            const ftk::Box2I& g = getGeometry();
            setViewZoom(value, ftk::V2I(g.w() / 2, g.h() / 2));
        }

        void TimelineWidget::setViewZoom(
            double zoom,
            const ftk::V2I& focus)
        {
            FTK_P();
            _setViewZoom(
                zoom,
                p.scale,
                focus,
                p.scrollWidget->getScrollPos());
        }

        void TimelineWidget::setViewZoomAtCurrentTime(double zoom)
        {
            FTK_P();
            const ftk::Box2I vp = p.scrollWidget->getScrollInfo().viewport;

            // Both zoom limits are measured from the viewport's width, so
            // before the first layout there is nothing to measure and any
            // zoom would be clamped away. A zoom from the command line
            // arrives then: remember it and apply it once the size is known.
            if (!p.player || vp.w() <= 1)
            {
                p.zoomPending = zoom;
                return;
            }
            const ftk::V2I scrollPos = p.scrollWidget->getScrollPos();
            const int x = p.ruler->timeToPos(p.currentTime) - vp.min.x;
            const int focus = x >= 0 && x <= vp.w() ? x : vp.w() / 2;

            // The current time's position in the content; scrolling there
            // less the focus puts it under the focus before the zoom.
            const int content = x + scrollPos.x;
            _setViewZoom(
                zoom,
                p.scale,
                ftk::V2I(focus, 0),
                ftk::V2I(content - focus, scrollPos.y));
        }

        void TimelineWidget::frameView()
        {
            FTK_P();
            p.scrollWidget->setScrollPos(ftk::V2I());
            const double scale = _getTimelineScale();
            if (scale != p.scale)
            {
                p.scale = scale;
                _setItemScale();
                setSizeUpdate();
                setDrawUpdate();
            }
        }

        bool TimelineWidget::hasFrameView() const
        {
            return _p->frameView->get();
        }

        std::shared_ptr<ftk::IObservable<bool> > TimelineWidget::observeFrameView() const
        {
            return _p->frameView;
        }

        void TimelineWidget::setFrameView(bool value)
        {
            FTK_P();
            if (p.frameView->setIfChanged(value))
            {
                if (value)
                {
                    frameView();
                }
            }
        }

        bool TimelineWidget::areScrollBarsVisible() const
        {
            return _p->scrollBarsVisible->get();
        }

        std::shared_ptr<ftk::IObservable<bool> > TimelineWidget::observeScrollBarsVisible() const
        {
            return _p->scrollBarsVisible;
        }

        void TimelineWidget::setScrollBarsVisible(bool value)
        {
            FTK_P();
            if (p.scrollBarsVisible->setIfChanged(value))
            {
                _p->scrollWidget->setScrollBarsVisible(value);
            }
        }

        bool TimelineWidget::hasAutoScroll() const
        {
            return _p->autoScroll->get();
        }

        std::shared_ptr<ftk::IObservable<bool> > TimelineWidget::observeAutoScroll() const
        {
            return _p->autoScroll;
        }

        void TimelineWidget::setAutoScroll(bool value)
        {
            FTK_P();
            if (p.autoScroll->setIfChanged(value))
            {
                _scrollUpdate();
            }
        }

        void TimelineWidget::setScrollBinding(ftk::MouseButton button, ftk::KeyModifier modifier)
        {
            _p->scrollBinding = std::make_pair(button, modifier);
        }

        void TimelineWidget::setMouseWheelScale(float value)
        {
            _p->mouseWheelScale = value;
        }

        bool TimelineWidget::hasStopOnScrub() const
        {
            return _p->stopOnScrub->get();
        }

        std::shared_ptr<ftk::IObservable<bool> > TimelineWidget::observeStopOnScrub() const
        {
            return _p->stopOnScrub;
        }

        void TimelineWidget::setStopOnScrub(bool value)
        {
            FTK_P();
            if (p.stopOnScrub->setIfChanged(value))
            {
                if (!p.timelineItems.empty())
                {
                    p.timelineItems.front()->setStopOnScrub(value);
                }
                p.ruler->setStopOnScrub(value);
            }
        }

        std::shared_ptr<ftk::IObservable<bool> > TimelineWidget::observeScrub() const
        {
            return _p->scrub;
        }

        std::shared_ptr<ftk::IObservable<std::optional<OTIO_NS::RationalTime> > > TimelineWidget::observeTimeScrub() const
        {
            return _p->timeScrub;
        }

        std::shared_ptr<ftk::IObservable<std::optional<OTIO_NS::RationalTime> > > TimelineWidget::observeTimeHover() const
        {
            return _p->timeHover;
        }

        const std::vector<int>& TimelineWidget::getFrameMarkers() const
        {
            return _p->frameMarkers;
        }

        void TimelineWidget::setFrameMarkers(const std::vector<int>& value)
        {
            FTK_P();
            if (value == p.frameMarkers)
                return;
            p.frameMarkers = value;
            p.ruler->setFrameMarkers(value);
        }

        void TimelineWidget::setHoles(const std::vector<OTIO_NS::TimeRange>& value)
        {
            FTK_P();
            if (value == p.holes)
                return;
            p.holes = value;
            setDrawUpdate();
        }

        void TimelineWidget::setTimeMarks(const std::vector<TimeMark>& value)
        {
            FTK_P();
            if (value == p.timeMarks)
                return;
            p.timeMarks = value;
            setDrawUpdate();
        }

        const std::vector<Marker>& TimelineWidget::getMarkers() const
        {
            return _p->markers;
        }

        void TimelineWidget::setMarkers(const std::vector<Marker>& value)
        {
            FTK_P();
            if (value == p.markers)
                return;
            p.markers = value;
            p.ruler->setMarkers(value);
        }

        const ItemColors& TimelineWidget::getItemColors(int index) const
        {
            FTK_P();
            static const ItemColors empty;
            return index >= 0 && index < static_cast<int>(p.itemColors.size()) ?
                p.itemColors[index] :
                empty;
        }

        void TimelineWidget::setItemColors(int index, const ItemColors& value)
        {
            FTK_P();
            if (index < 0)
                return;
            if (index >= static_cast<int>(p.itemColors.size()))
            {
                p.itemColors.resize(index + 1);
            }
            if (value == p.itemColors[index])
                return;
            p.itemColors[index] = value;
            if (index < static_cast<int>(p.timelineItems.size()))
            {
                p.timelineItems[index]->setItemColors(value);
            }
        }

        void TimelineWidget::setLabel(int index, const std::string& value)
        {
            FTK_P();
            if (index < 0)
                return;
            if (index >= static_cast<int>(p.labels.size()))
            {
                p.labels.resize(index + 1);
            }
            if (value == p.labels[index])
                return;
            p.labels[index] = value;
            _labelsUpdate();
        }

        void TimelineWidget::setOffset(
            int index,
            const OTIO_NS::RationalTime& value)
        {
            FTK_P();
            if (index < 0)
                return;
            if (index >= static_cast<int>(p.offsets.size()))
            {
                p.offsets.resize(index + 1);
            }
            if (compareExact(value, p.offsets[index]))
                return;
            p.offsets[index] = value;
            _offsetsUpdate();
        }

        void TimelineWidget::_offsetsUpdate()
        {
            FTK_P();
            for (size_t i = 0; i < p.timelineItems.size(); ++i)
            {
                p.timelineItems[i]->setOffset(
                    i < p.offsets.size() ?
                    p.offsets[i] :
                    OTIO_NS::RationalTime());
            }

            // The ruler names the player's times, so it moves with the
            // player's timeline and the times stay over the frames they name.
            p.ruler->setOffset(!p.offsets.empty() ?
                p.offsets.front() :
                OTIO_NS::RationalTime());
        }

        void TimelineWidget::_labelsUpdate()
        {
            FTK_P();

            // Nothing to tell apart when there is only one.
            const bool several = p.timelineItems.size() > 1;
            for (size_t i = 0; i < p.timelineItems.size(); ++i)
            {
                std::string label;
                if (several)
                {
                    label = i < p.labels.size() ? p.labels[i] : std::string();
                    if (label.empty())
                    {
                        const auto& timeline = 0 == i ?
                            p.player->getTimeline() :
                            p.compare[i - 1];
                        label = timeline->getPath().getFileName();
                    }
                }
                p.timelineItems[i]->setLabel(label);
            }
        }

        const ItemOptions& TimelineWidget::getItemOptions() const
        {
            return _p->itemOptions->get();
        }

        std::shared_ptr<ftk::IObservable<ItemOptions> > TimelineWidget::observeItemOptions() const
        {
            return _p->itemOptions;
        }

        void TimelineWidget::setItemOptions(const ItemOptions& value)
        {
            FTK_P();
            if (p.itemOptions->setIfChanged(value))
            {
                for (const auto& item : p.timelineItems)
                {
                    item->setOptions(value);
                }
                p.ruler->setOptions(value);
            }
        }

        const DisplayOptions& TimelineWidget::getDisplayOptions() const
        {
            return _p->displayOptions->get();
        }

        std::shared_ptr<ftk::IObservable<DisplayOptions> > TimelineWidget::observeDisplayOptions() const
        {
            return _p->displayOptions;
        }

        void TimelineWidget::setVideoCacheColor(const std::optional<ftk::Color4F>& value)
        {
            _p->ruler->setVideoCacheColor(value);
        }

        void TimelineWidget::setDisplayOptions(const DisplayOptions& value)
        {
            FTK_P();
            if (p.displayOptions->setIfChanged(value))
            {
                p.scrollWidget->getScrollArea()->setBackgroundRole(
                    value.background);
                for (const auto& item : p.timelineItems)
                {
                    item->setDisplayOptions(value);
                }
                p.ruler->setDisplayOptions(value);
                _scrollUpdate();
            }
        }
        
        void TimelineWidget::refreshMedia()
        {
            FTK_P();
            for (const auto& item : p.timelineItems)
            {
                item->refreshMedia();
            }
        }

        ftk::Size2I TimelineWidget::getSizeHint() const
        {
            FTK_P();
            ftk::Size2I out = p.scrollWidget->getSizeHint();
            out.h += p.ruler->getSizeHint().h;
            return out;
        }

        void TimelineWidget::setGeometry(const ftk::Box2I& value)
        {
            IWidget::setGeometry(value);
            FTK_P();
            const int rulerHeight = p.ruler->getSizeHint().h;
            p.scrollWidget->setGeometry(ftk::Box2I(
                value.min.x,
                value.min.y + rulerHeight,
                value.w(),
                std::max(0, value.h() - rulerHeight)));

            // After the scroll area, whose viewport is what says how much
            // width the scroll bar left for the timelines.
            const ftk::Box2I viewport = p.scrollWidget->getScrollInfo().viewport;
            p.ruler->setGeometry(ftk::Box2I(
                viewport.min.x,
                value.min.y,
                viewport.w(),
                rulerHeight));

            if (p.sizeInit ||
                (p.frameView->get() && viewport.w() != p.frameViewWidth))
            {
                p.sizeInit = false;
                p.frameViewWidth = viewport.w();
                frameView();
            }
            else if (!p.timelineItems.empty() &&
                p.scale < _getTimelineScale())
            {
                // Zoomed out past the whole of them, so there is nothing left
                // to look closely at and framing them is what was wanted.
                // Asked of the scale rather than the laid out width: at the
                // framed scale the two are the same size give or take the
                // margins, so a width comparison turns framing back on every
                // layout, and no zoom or unchecked Frame View would hold.
                setFrameView(true);
                p.frameViewWidth = viewport.w();
                frameView();
            }

            // A zoom that arrived before this widget had a size.
            if (p.zoomPending.has_value() && p.player && viewport.w() > 1)
            {
                const double zoom = p.zoomPending.value();
                p.zoomPending.reset();
                setViewZoomAtCurrentTime(zoom);
            }
        }

        void TimelineWidget::sizeHintEvent(const ftk::SizeHintEvent& event)
        {
            FTK_P();
            p.sizeInit |= event.displayScale != p.displayScale;
            p.displayScale = event.displayScale;
            p.border = event.style->getSizeRole(
                ftk::SizeRole::Border, event.displayScale);
        }

        void TimelineWidget::drawOverlayEvent(
            const ftk::Box2I& drawRect,
            const ftk::DrawEvent& event)
        {
            IWidget::drawOverlayEvent(drawRect, event);
            FTK_P();
            if (!p.player)
                return;

            // Over every timeline rather than only the one being played: the
            // same instant marked down the stack is what says where the
            // versions are being compared.
            const ftk::Box2I& g = p.scrollWidget->getGeometry();
            if (!p.holes.empty())
            {
                // Under the ruler, over the tracks: dimmed, and striped at
                // 45 degrees light and dark in turn, one of which stands
                // out from any picture.
                const int y0 = std::max(g.min.y, p.ruler->getGeometry().max.y + 1);
                const int y1 = g.max.y;
                const int h = y1 - y0 + 1;
                ftk::Color4F dim = event.style->getColorRole(ftk::ColorRole::Window);
                dim.a = .35F;
                const int spacing = std::max(2, p.border * 5);
                ftk::LineOptions lineOptions;
                lineOptions.width = std::max(1.F, p.border * 1.5F);
                std::vector<std::pair<ftk::V2F, ftk::V2F> > light;
                std::vector<std::pair<ftk::V2F, ftk::V2F> > dark;
                for (const auto& hole : p.holes)
                {
                    const int x0 = std::max(g.min.x, p.ruler->timeToPos(hole.start_time()));
                    const int x1 = std::min(g.max.x, p.ruler->timeToPos(hole.end_time_exclusive()));
                    if (x1 <= x0 || h <= 0)
                        continue;
                    event.render->drawRect(ftk::Box2I(x0, y0, x1 - x0, h), dim);
                    // Lines from the bottom left up to the right, clipped
                    // to the hole; anchored to the timeline, not the hole,
                    // so they do not crawl as it grows.
                    const int origin = p.ruler->timeToPos(p.timeRange.start_time());
                    int k = (x0 - h - origin) / spacing - 1;
                    for (int xa = origin + k * spacing; xa < x1; xa += spacing, ++k)
                    {
                        float ax = static_cast<float>(xa);
                        float ay = static_cast<float>(y1);
                        float bx = static_cast<float>(xa + h);
                        float by = static_cast<float>(y0);
                        if (ax < x0)
                        {
                            ay -= x0 - ax;
                            ax = static_cast<float>(x0);
                        }
                        if (bx > x1)
                        {
                            by += bx - x1;
                            bx = static_cast<float>(x1);
                        }
                        if (bx <= ax)
                            continue;
                        (k % 2 ? light : dark).push_back({ ftk::V2F(ax, ay), ftk::V2F(bx, by) });
                    }
                }
                if (!light.empty())
                {
                    event.render->drawLines(light, ftk::Color4F(1.F, 1.F, 1.F, .55F), lineOptions);
                }
                if (!dark.empty())
                {
                    event.render->drawLines(dark, ftk::Color4F(0.F, 0.F, 0.F, .65F), lineOptions);
                }
            }
            if (!compareExact(p.inOutRange, p.timeRange) && p.inOutRange.duration().value() > 0.0)
            {
                // Outside, faded towards the background: left out of
                // playback.
                const int x0 = p.ruler->timeToPos(p.inOutRange.start_time());
                const int x1 = p.ruler->timeToPos(p.inOutRange.end_time_exclusive());
                ftk::Color4F fade = event.style->getColorRole(ftk::ColorRole::Window);
                fade.a = .6F;
                if (x0 > g.min.x)
                {
                    event.render->drawRect(
                        ftk::Box2I(g.min.x, g.min.y, std::min(x0, g.max.x + 1) - g.min.x, g.h()),
                        fade);
                }
                if (x1 <= g.max.x)
                {
                    const int xs = std::max(x1, g.min.x);
                    event.render->drawRect(
                        ftk::Box2I(xs, g.min.y, g.max.x + 1 - xs, g.h()),
                        fade);
                }
                // The brackets, each fading inwards over ten lines as the
                // refresh marks do, with a foot at the top and the bottom.
                const ftk::Color4F bracket(.35F, .6F, 1.F);
                for (int i = 0; i < 10; ++i)
                {
                    ftk::Color4F color = bracket;
                    color.a = 1.F - i / 10.F;
                    const int xa = x0 + i * p.border;
                    if (xa >= g.min.x && xa <= g.max.x)
                    {
                        event.render->drawRect(ftk::Box2I(xa, g.min.y, p.border, g.h()), color);
                    }
                    const int xb = x1 - p.border - i * p.border;
                    if (xb >= g.min.x && xb <= g.max.x)
                    {
                        event.render->drawRect(ftk::Box2I(xb, g.min.y, p.border, g.h()), color);
                    }
                }
                const int foot = p.border * 6;
                const int footH = p.border * 2;
                event.render->drawRect(ftk::Box2I(x0, g.min.y, foot, footH), bracket);
                event.render->drawRect(ftk::Box2I(x0, g.max.y + 1 - footH, foot, footH), bracket);
                event.render->drawRect(ftk::Box2I(x1 - foot, g.min.y, foot, footH), bracket);
                event.render->drawRect(ftk::Box2I(x1 - foot, g.max.y + 1 - footH, foot, footH), bracket);
            }
            for (const auto& timeMark : p.timeMarks)
            {
                const int x = p.ruler->timeToPos(timeMark.time);
                for (int i = 0; i < 10; ++i)
                {
                    const int xi = x + i * p.border;
                    if (xi >= g.min.x && xi <= g.max.x)
                    {
                        ftk::Color4F color = timeMark.color;
                        color.a *= 1.F - i / 10.F;
                        event.render->drawRect(
                            ftk::Box2I(xi, g.min.y, p.border, g.h()),
                            color);
                    }
                }
            }
            const int x = p.ruler->timeToPos(p.currentTime);
            if (x >= g.min.x && x <= g.max.x)
            {
                event.render->drawRect(
                    ftk::Box2I(x, g.min.y, p.border * 2, g.h()),
                    event.style->getColorRole(ftk::ColorRole::Red));
            }
        }

        void TimelineWidget::mouseEnterEvent(ftk::MouseEnterEvent& event)
        {
            FTK_P();
            event.accept = true;
            p.mouse.inside = true;
        }

        void TimelineWidget::mouseLeaveEvent()
        {
            FTK_P();
            p.mouse.inside = false;
        }

        OTIO_NS::RationalTime TimelineWidget::_frameAt(int x) const
        {
            FTK_P();
            const double rate = p.timeRange.duration().rate();
            return ftk::clamp(
                p.ruler->posToTime(x).rescaled_to(rate).floor(),
                p.timeRange.start_time().rescaled_to(rate),
                p.timeRange.end_time_inclusive().rescaled_to(rate));
        }

        void TimelineWidget::mouseMoveEvent(ftk::MouseMoveEvent& event)
        {
            FTK_P();
            switch (p.mouse.mode)
            {
            case Private::MouseMode::Scroll:
            {
                event.accept = true;
                const ftk::V2I d = event.pos - p.mouse.press;
                p.scrollWidget->setScrollPos(p.mouse.scrollPos - d);
                setFrameView(false);
                break;
            }
            case Private::MouseMode::InOutSet:
            {
                event.accept = true;
                if (!p.mouse.inOutDragged &&
                    std::abs(event.pos.x - p.mouse.press.x) > p.border * 3)
                {
                    p.mouse.inOutDragged = true;
                }
                if (p.mouse.inOutDragged && p.player)
                {
                    const OTIO_NS::RationalTime t = _frameAt(event.pos.x);
                    p.player->setInOutRange(OTIO_NS::TimeRange::range_from_start_end_time_inclusive(
                        std::min(t, p.mouse.inOutAnchor),
                        std::max(t, p.mouse.inOutAnchor)));
                }
                break;
            }
            case Private::MouseMode::InOutStart:
            case Private::MouseMode::InOutEnd:
            {
                event.accept = true;
                if (p.player)
                {
                    const OTIO_NS::RationalTime t = _frameAt(event.pos.x);
                    const OTIO_NS::RationalTime start = p.inOutRange.start_time();
                    const OTIO_NS::RationalTime end = p.inOutRange.end_time_inclusive();
                    p.player->setInOutRange(
                        Private::MouseMode::InOutStart == p.mouse.mode ?
                        OTIO_NS::TimeRange::range_from_start_end_time_inclusive(std::min(t, end), end) :
                        OTIO_NS::TimeRange::range_from_start_end_time_inclusive(start, std::max(t, start)));
                }
                break;
            }
            default: break;
            }
        }

        void TimelineWidget::mousePressEvent(ftk::MouseClickEvent& event)
        {
            FTK_P();
            p.mouse.press = event.pos;
            if (p.itemOptions->get().inputEnabled &&
                p.scrollBinding.first == event.button &&
                checkKeyModifier(p.scrollBinding.second, event.modifiers))
            {
                // Only claim the button when it is bound to scrolling.
                // Accepting a button we do nothing with would deny it to
                // everything else, including a context menu.
                event.accept = true;
                p.mouse.mode = Private::MouseMode::Scroll;
                p.mouse.scrollPos = p.scrollWidget->getScrollPos();
            }
            else if (p.itemOptions->get().inputEnabled &&
                p.player &&
                ftk::MouseButton::Left == event.button &&
                static_cast<int>(ftk::KeyModifier::Shift) == event.modifiers)
            {
                // Shift and the left button: an A-B range, from here.
                event.accept = true;
                p.mouse.mode = Private::MouseMode::InOutSet;
                p.mouse.inOutAnchor = _frameAt(event.pos.x);
                p.mouse.inOutDragged = false;
            }
            else if (p.itemOptions->get().inputEnabled &&
                p.player &&
                ftk::MouseButton::Left == event.button &&
                0 == event.modifiers &&
                !compareExact(p.inOutRange, p.timeRange))
            {
                // A bracket, left to here by the tracks and the ruler: the
                // nearer of the two.
                p.mouse.mode = Private::MouseMode::None;
                const int grab = p.border * 5;
                const int d0 = std::abs(event.pos.x - p.ruler->timeToPos(p.inOutRange.start_time()));
                const int d1 = std::abs(event.pos.x - p.ruler->timeToPos(p.inOutRange.end_time_exclusive()));
                if (std::min(d0, d1) <= grab)
                {
                    event.accept = true;
                    p.mouse.mode = d0 <= d1 ?
                        Private::MouseMode::InOutStart :
                        Private::MouseMode::InOutEnd;
                }
            }
            else
            {
                p.mouse.mode = Private::MouseMode::None;
            }
        }

        void TimelineWidget::mouseReleaseEvent(ftk::MouseClickEvent& event)
        {
            FTK_P();
            event.accept = true;
            if (Private::MouseMode::Scroll == p.mouse.mode && event.cancel)
            {
                // The first finger of a gesture was not scrolling.
                p.scrollWidget->setScrollPos(p.mouse.scrollPos);
            }
            // Shift and a click without a drag: the whole timeline again.
            if (Private::MouseMode::InOutSet == p.mouse.mode &&
                !p.mouse.inOutDragged &&
                !event.cancel &&
                p.player)
            {
                p.player->setInOutRange(p.timeRange);
            }
            p.mouse.mode = Private::MouseMode::None;
        }

        void TimelineWidget::scrollEvent(ftk::ScrollEvent& event)
        {
            FTK_P();
            if (p.itemOptions->get().inputEnabled)
            {
                event.accept = true;
                const double newZoom =
                    event.value.y > 0 ?
                    p.scale * p.mouseWheelScale :
                    p.scale / p.mouseWheelScale;
                setViewZoom(newZoom, event.pos);
            }
        }

        void TimelineWidget::gestureEvent(ftk::GestureEvent& event)
        {
            FTK_P();
            if (p.itemOptions->get().inputEnabled)
            {
                event.accept = true;

                // Two fingers drag the time and the tracks, with what is
                // left over from rounding carried to the next event, or
                // fingers moving slowly would move nothing.
                p.gesturePan = p.gesturePan + event.pan;
                const ftk::V2I pan(p.gesturePan.x, p.gesturePan.y);
                p.gesturePan = p.gesturePan - ftk::V2F(pan.x, pan.y);
                if (pan.x != 0 || pan.y != 0)
                {
                    p.scrollWidget->setScrollPos(p.scrollWidget->getScrollPos() - pan);
                    setFrameView(false);
                }

                // Pinching zooms the time around the point between them.
                if (event.zoom != 1.F)
                {
                    const ftk::Box2I vp = p.scrollWidget->getScrollInfo().viewport;
                    setViewZoom(
                        p.scale * event.zoom,
                        ftk::V2I(event.pos.x - vp.min.x, event.pos.y - vp.min.y));
                }
            }
        }

        void TimelineWidget::keyPressEvent(ftk::KeyEvent& event)
        {
            FTK_P();
            if (p.itemOptions->get().inputEnabled &&
                0 == event.modifiers)
            {
                switch (event.key)
                {
                case ftk::Key::Equals:
                    event.accept = true;
                    setViewZoom(p.scale * 2.F, event.pos);
                    break;
                case ftk::Key::Minus:
                    event.accept = true;
                    setViewZoom(p.scale / 2.F, event.pos);
                    break;
                case ftk::Key::Backspace:
                    event.accept = true;
                    setFrameView(true);
                    break;
                default: break;
                }
            }
        }

        void TimelineWidget::keyReleaseEvent(ftk::KeyEvent& event)
        {
            event.accept = true;
        }

        void TimelineWidget::_setViewZoom(
            double zoomNew,
            double zoomPrev,
            const ftk::V2I& focus,
            const ftk::V2I& scrollPos)
        {
            FTK_P();
            const double zoomMin = _getTimelineScale();
            const double zoomMax = _getTimelineScaleMax();
            const double zoomClamped = ftk::clamp(zoomNew, zoomMin, zoomMax);
            if (zoomClamped != p.scale)
            {
                p.scale = zoomClamped;
                _setItemScale();
                const double s = zoomClamped / zoomPrev;
                const ftk::V2I scrollPosNew(
                    (scrollPos.x + focus.x) * s - focus.x,
                    scrollPos.y);
                p.scrollWidget->setScrollPos(scrollPosNew, false);

                setFrameView(zoomNew <= zoomMin);
            }
        }

        double TimelineWidget::_getDuration() const
        {
            FTK_P();

            // The longest of them, so that framing the view frames all of
            // them: a shorter timeline stops early and leaves the rest of its
            // row empty, which is the difference being looked for.
            double out = 0.0;
            if (p.player)
            {
                out = p.player->getTimeRange().duration().rescaled_to(1.0).value();
            }
            for (const auto& timeline : p.compare)
            {
                out = std::max(
                    out,
                    timeline->getTimeRange().duration().rescaled_to(1.0).value());
            }
            return out;
        }

        double TimelineWidget::_getTimelineScale() const
        {
            FTK_P();
            double out = 1.0;
            if (p.player)
            {
                const double duration = _getDuration();
                if (duration > 0.0)
                {
                    const ftk::Box2I scrollViewport = p.scrollWidget->getScrollInfo().viewport;
                    out = scrollViewport.w() / duration;
                }
            }
            return out;
        }

        double TimelineWidget::_getTimelineScaleMax() const
        {
            FTK_P();
            double out = 1.0;
            if (p.player)
            {
                const ftk::Box2I scrollViewport = p.scrollWidget->getScrollInfo().viewport;
                const double duration = _getDuration();
                if (duration < 1.0)
                {
                    if (duration > 0.0)
                    {
                        out = scrollViewport.w() / duration;
                    }
                }
                else
                {
                    out = scrollViewport.w();
                }
            }
            return out;
        }

        void TimelineWidget::_setItemScale()
        {
            FTK_P();
            for (const auto& item : p.timelineItems)
            {
                item->setScale(p.scale);
            }
            p.ruler->setScale(p.scale);
            p.ruler->setScrollPos(p.scrollWidget->getScrollPos().x);
        }

        void TimelineWidget::_scrollUpdate()
        {
            FTK_P();
            // Follows the player's current time; the timelines it is compared
            // against have no playhead of their own to follow.
            if (!p.timelineItems.empty() &&
                p.autoScroll->get() &&
                !p.scrub->get() &&
                Private::MouseMode::None == p.mouse.mode)
            {
                ftk::V2I scrollPos = p.scrollWidget->getScrollPos();
                const OTIO_NS::RationalTime t = p.currentTime - p.timeRange.start_time();
                const int pos =
                    getGeometry().min.x -
                    scrollPos.x +
                    t.rescaled_to(1.0).value() * p.scale;

                const ftk::Box2I vp = p.scrollWidget->getScrollInfo().viewport;
                const int margin = vp.w() * marginPercentage;
                if (pos < (vp.min.x + margin) || pos >(vp.max.x - margin))
                {
                    const int offset = pos < (vp.min.x + margin) ? (vp.min.x + margin) : (vp.max.x - margin);
                    scrollPos.x = getGeometry().min.x - offset + t.rescaled_to(1.0).value() * p.scale;
                    p.scrollWidget->setScrollPos(scrollPos);
                }
            }
            p.scrollWidget->setScrollType(p.displayOptions->get().minimize ?
                ftk::ScrollType::Horizontal :
                ftk::ScrollType::Both);
        }

        std::shared_ptr<ItemData> TimelineWidget::_getItemData(
            const std::shared_ptr<Timeline>& timeline) const
        {
            FTK_P();
            auto out = std::make_shared<ItemData>();
            out->timeUnitsModel = p.timeUnitsModel;
            out->speed = timeline->getTimeRange().duration().rate();
            out->dir = timeline->getPath().getProtocol() +
                timeline->getPath().getDir();
            out->options = timeline->getOptions();

            // Only where media times are one number for the whole timeline.
            // On a cut list they restart at every cut, so the ruler would
            // count up to each cut and start again, and a clip whose media
            // runs at a different rate than the timeline would label two of
            // its frames the same. Left unset the ruler counts the timeline.
            if (timeline->isMediaTimeContinuous())
            {
                // Weak, because the item data belongs to this widget and the
                // timeline does not.
                std::weak_ptr<Timeline> weak = timeline;
                out->toMediaTime =
                    [weak](const OTIO_NS::RationalTime& value)
                    {
                        if (auto timeline = weak.lock())
                        {
                            if (const auto time = timeline->getMediaTime(value))
                            {
                                return *time;
                            }
                        }
                        return value;
                    };
            }
            return out;
        }

        void TimelineWidget::_timelineUpdate()
        {
            FTK_P();

            const ftk::V2I scrollPos = p.scrollWidget->getScrollPos();

            p.scrubObserver.reset();
            p.timeScrubObserver.reset();
            p.timeHoverObserver.reset();
            p.timeHover->setIfChanged(std::nullopt);
            for (const auto& item : p.timelineItems)
            {
                item->setParent(nullptr);
            }
            p.timelineItems.clear();
            p.itemData.clear();
            p.ruler->setPlayer(nullptr);

            // Different timelines are a different width to frame.
            p.frameViewWidth = -1;

            if (p.player)
            {
                if (auto context = getContext())
                {
                    // The player's own timeline first, then what it is being
                    // compared against, so that the item index and the compare
                    // index agree with each other.
                    p.itemData.push_back(_getItemData(p.player->getTimeline()));
                    p.timelineItems.push_back(TimelineItem::create(
                        context,
                        p.player,
                        p.scale,
                        p.itemOptions->get(),
                        p.displayOptions->get(),
                        p.itemData.back(),
                        p.layout));
                    for (const auto& timeline : p.compare)
                    {
                        p.itemData.push_back(_getItemData(timeline));
                        p.timelineItems.push_back(TimelineItem::create(
                            context,
                            timeline,
                            p.scale,
                            p.itemOptions->get(),
                            p.displayOptions->get(),
                            p.itemData.back(),
                            p.layout));
                    }

                    p.timelineItems.front()->setStopOnScrub(p.stopOnScrub->get());
                    p.timelineItems.front()->setFrameMarkers(p.frameMarkers);
                    for (size_t i = 0; i < p.timelineItems.size(); ++i)
                    {
                        if (i < p.itemColors.size())
                        {
                            p.timelineItems[i]->setItemColors(p.itemColors[i]);
                        }
                    }
                    _labelsUpdate();
                    _offsetsUpdate();

                    // The ruler is the player's, so it names its times the
                    // way the player's own timeline does.
                    p.ruler->setItemData(p.itemData.front());
                    p.ruler->setPlayer(p.player);
                    p.ruler->setScale(p.scale);
                    p.ruler->setFrameMarkers(p.frameMarkers);
                    p.ruler->setMarkers(p.markers);
                    p.ruler->setDisplayOptions(p.displayOptions->get());
                    p.ruler->setOptions(p.itemOptions->get());
                    p.ruler->setStopOnScrub(p.stopOnScrub->get());

                    p.scrollWidget->setScrollPos(scrollPos);

                    // Only the player's item can be scrubbed, so it is the
                    // only one there is anything to hear from.
                    p.scrubObserver = ftk::Observer<bool>::create(
                        p.timelineItems.front()->observeScrub(),
                        [this](bool value)
                        {
                            _p->scrub->setIfChanged(value);
                            _scrollUpdate();
                        });

                    p.timeScrubObserver = ftk::Observer<std::optional<OTIO_NS::RationalTime> >::create(
                        p.timelineItems.front()->observeTimeScrub(),
                        [this](const std::optional<OTIO_NS::RationalTime>& value)
                        {
                            _p->timeScrub->setIfChanged(value);
                        });

                    p.timeHoverObserver = ftk::Observer<std::optional<OTIO_NS::RationalTime> >::create(
                        p.timelineItems.front()->observeTimeHover(),
                        [this](const std::optional<OTIO_NS::RationalTime>& value)
                        {
                            _p->timeHover->setIfChanged(value);
                        });
                }
            }
        }
    }
}
