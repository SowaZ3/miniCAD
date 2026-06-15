#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <gl/GL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "opengl32.lib")

namespace
{
    const double kPi = 3.14159265358979323846;
    const double kEpsilon = 1e-6;
    const double kMinRadius = 6.0;
    const double kSnapDistance = 10.0;
    const double kMinZoom = 0.1;
    const double kMaxZoom = 20.0;
    const int kDefaultWidth = 980;
    const int kDefaultHeight = 720;

    const UINT ID_MODE_LINE = 1001;
    const UINT ID_MODE_ARC = 1002;
    const UINT ID_FINISH = 1004;
    const UINT ID_CONTINUE = 1005;
    const UINT ID_UNDO = 1006;
    const UINT ID_REDO = 1007;
    const UINT ID_DELETE_SEGMENT = 1008;
    const UINT ID_EXPORT_SVG = 1009;
    const UINT ID_FILE_NEW = 1010;
    const UINT ID_FILE_OPEN = 1011;
    const UINT ID_FILE_SAVE = 1012;
    const UINT ID_FILE_SAVE_AS = 1013;
    const UINT ID_FILE_EXIT = 1014;

    struct Vec2
    {
        double x;
        double y;

        Vec2() : x(0.0), y(0.0) {}
        Vec2(double xValue, double yValue) : x(xValue), y(yValue) {}
    };

    Vec2 operator+(const Vec2& lhs, const Vec2& rhs)
    {
        return Vec2(lhs.x + rhs.x, lhs.y + rhs.y);
    }

    Vec2 operator-(const Vec2& lhs, const Vec2& rhs)
    {
        return Vec2(lhs.x - rhs.x, lhs.y - rhs.y);
    }

    Vec2 operator*(const Vec2& value, double scalar)
    {
        return Vec2(value.x * scalar, value.y * scalar);
    }

    Vec2 operator/(const Vec2& value, double scalar)
    {
        return Vec2(value.x / scalar, value.y / scalar);
    }

    double Dot(const Vec2& lhs, const Vec2& rhs)
    {
        return lhs.x * rhs.x + lhs.y * rhs.y;
    }

    double Cross(const Vec2& lhs, const Vec2& rhs)
    {
        return lhs.x * rhs.y - lhs.y * rhs.x;
    }

    double Length(const Vec2& value)
    {
        return std::sqrt(Dot(value, value));
    }

    Vec2 Normalize(const Vec2& value)
    {
        const double length = Length(value);
        if (length <= kEpsilon)
        {
            return Vec2();
        }

        return value / length;
    }

    Vec2 PerpendicularLeft(const Vec2& value)
    {
        return Vec2(-value.y, value.x);
    }

    Vec2 PerpendicularRight(const Vec2& value)
    {
        return Vec2(value.y, -value.x);
    }

    double Distance(const Vec2& lhs, const Vec2& rhs)
    {
        return Length(lhs - rhs);
    }

    enum SegmentType
    {
        SegmentLine,
        SegmentArc
    };

    struct Segment
    {
        SegmentType type;
        Vec2 start;
        Vec2 end;
        Vec2 center;
        bool clockwise;

        Segment() : type(SegmentLine), clockwise(false) {}
    };

    struct Polyline
    {
        Vec2 startPoint;
        std::vector<Segment> segments;
        bool finished;

        Polyline() : finished(false) {}
    };

    struct EditorState
    {
        std::vector<Polyline> polylines;
        int activePolylineIndex;
        bool hasPendingArcEnd;
        Vec2 pendingArcEnd;
        std::string distanceInputBuffer;
        bool hasDistanceDirection;
        Vec2 distanceDirection;
    };

    struct SegmentSelection
    {
        int polylineIndex;
        int segmentIndex;

        SegmentSelection() : polylineIndex(-1), segmentIndex(-1) {}
    };

    struct Bounds
    {
        Vec2 min;
        Vec2 max;
        bool initialized;

        Bounds() : min(), max(), initialized(false) {}
    };

    enum InsertMode
    {
        InsertLine,
        InsertArc
    };

    HWND gWindow = NULL;
    HDC gWindowDC = NULL;
    HGLRC gGlContext = NULL;
    GLuint gHudFontBase = 0;
    HFONT gHudFont = NULL;
    int gClientWidth = kDefaultWidth;
    int gClientHeight = kDefaultHeight;

    std::vector<Polyline> gPolylines;
    int gActivePolylineIndex = -1;
    std::vector<EditorState> gUndoStates;
    std::vector<EditorState> gRedoStates;

    InsertMode gInsertMode = InsertLine;
    bool gHasPendingArcEnd = false;
    Vec2 gPendingArcEnd;
    Vec2 gMousePosition(0.0, 0.0);
    bool gHasHighlightedSnapPoint = false;
    Vec2 gHighlightedSnapPoint;
    std::string gDistanceInputBuffer;
    bool gHasDistanceDirection = false;
    Vec2 gDistanceDirection;
    POINT gMouseScreenPosition = { 0, 0 };
    Vec2 gViewOrigin(0.0, 0.0);
    double gZoom = 1.0;
    bool gIsPanning = false;
    POINT gLastPanScreenPosition = { 0, 0 };
    SegmentSelection gSelectedSegment;
    std::string gCurrentFilePath;

    double GetArcSweep(const Segment& segment);
    void UpdateMainMenuState();

    Vec2 GetCurrentEndPoint(const Polyline& polyline)
    {
        if (polyline.segments.empty())
        {
            return polyline.startPoint;
        }

        return polyline.segments.back().end;
    }

    Vec2 ScreenToWorld(double screenX, double screenY)
    {
        return Vec2(
            gViewOrigin.x + screenX / gZoom,
            gViewOrigin.y + screenY / gZoom
        );
    }

    Vec2 ScreenToWorld(const POINT& screenPoint)
    {
        return ScreenToWorld(static_cast<double>(screenPoint.x), static_cast<double>(screenPoint.y));
    }

    Vec2 WorldToScreen(const Vec2& worldPoint)
    {
        return Vec2(
            (worldPoint.x - gViewOrigin.x) * gZoom,
            (worldPoint.y - gViewOrigin.y) * gZoom
        );
    }

    double GetHandleRadiusWorld()
    {
        return 4.0 / gZoom;
    }

    bool HasSelectedSegment()
    {
        return gSelectedSegment.polylineIndex >= 0 &&
               gSelectedSegment.polylineIndex < static_cast<int>(gPolylines.size()) &&
               gSelectedSegment.segmentIndex >= 0 &&
               gSelectedSegment.segmentIndex < static_cast<int>(gPolylines[gSelectedSegment.polylineIndex].segments.size());
    }

    void ClearSelectedSegment()
    {
        gSelectedSegment = SegmentSelection();
    }

    void ExpandBounds(Bounds& bounds, const Vec2& point)
    {
        if (!bounds.initialized)
        {
            bounds.min = point;
            bounds.max = point;
            bounds.initialized = true;
            return;
        }

        bounds.min.x = std::min(bounds.min.x, point.x);
        bounds.min.y = std::min(bounds.min.y, point.y);
        bounds.max.x = std::max(bounds.max.x, point.x);
        bounds.max.y = std::max(bounds.max.y, point.y);
    }

    std::string FormatSvgNumber(double value)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(3) << value;
        return stream.str();
    }

    std::string FormatPointForHud(const Vec2& point)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2);
        stream << "(" << point.x << ", " << point.y << ")";
        return stream.str();
    }

    std::string GetDisplayFileName()
    {
        if (gCurrentFilePath.empty())
        {
            return "Untitled";
        }

        const std::size_t slashPosition = gCurrentFilePath.find_last_of("\\/");
        return slashPosition == std::string::npos
            ? gCurrentFilePath
            : gCurrentFilePath.substr(slashPosition + 1);
    }

    void ResetEditorDocumentState()
    {
        gPolylines.clear();
        gActivePolylineIndex = -1;
        gUndoStates.clear();
        gRedoStates.clear();
        gHasPendingArcEnd = false;
        gPendingArcEnd = Vec2();
        gDistanceInputBuffer.clear();
        gHasDistanceDirection = false;
        gDistanceDirection = Vec2();
        ClearSelectedSegment();
        gHasHighlightedSnapPoint = false;
    }

    double DistanceToLineSegmentScreen(const Vec2& point, const Vec2& start, const Vec2& end)
    {
        const Vec2 pointScreen = WorldToScreen(point);
        const Vec2 startScreen = WorldToScreen(start);
        const Vec2 endScreen = WorldToScreen(end);
        const Vec2 segment = endScreen - startScreen;
        const double lengthSquared = Dot(segment, segment);
        if (lengthSquared <= kEpsilon)
        {
            return Distance(pointScreen, startScreen);
        }

        const double t = std::max(0.0, std::min(1.0, Dot(pointScreen - startScreen, segment) / lengthSquared));
        const Vec2 projection = startScreen + segment * t;
        return Distance(pointScreen, projection);
    }

    double DistanceToArcScreen(const Vec2& point, const Segment& segment)
    {
        const Vec2 radiusVector = segment.start - segment.center;
        const double radius = Length(radiusVector);
        const double startAngle = std::atan2(radiusVector.y, radiusVector.x);
        const double sweep = GetArcSweep(segment);
        const int steps = std::max(24, static_cast<int>(std::fabs(sweep) * radius / 8.0));

        double bestDistance = 1e9;
        Vec2 previousPoint = segment.start;
        for (int i = 1; i <= steps; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(steps);
            const double angle = startAngle + sweep * t;
            const Vec2 currentPoint(
                segment.center.x + std::cos(angle) * radius,
                segment.center.y + std::sin(angle) * radius
            );
            bestDistance = std::min(bestDistance, DistanceToLineSegmentScreen(point, previousPoint, currentPoint));
            previousPoint = currentPoint;
        }

        return bestDistance;
    }

    bool TrySelectSegmentAtPoint(const Vec2& point)
    {
        const double hitThreshold = 8.0;
        bool found = false;
        double bestDistance = hitThreshold;
        SegmentSelection bestSelection;

        for (int polylineIndex = 0; polylineIndex < static_cast<int>(gPolylines.size()); ++polylineIndex)
        {
            const Polyline& polyline = gPolylines[polylineIndex];
            for (int segmentIndex = 0; segmentIndex < static_cast<int>(polyline.segments.size()); ++segmentIndex)
            {
                const Segment& segment = polyline.segments[segmentIndex];
                const double distance = segment.type == SegmentLine
                    ? DistanceToLineSegmentScreen(point, segment.start, segment.end)
                    : DistanceToArcScreen(point, segment);

                if (distance <= bestDistance)
                {
                    bestDistance = distance;
                    bestSelection.polylineIndex = polylineIndex;
                    bestSelection.segmentIndex = segmentIndex;
                    found = true;
                }
            }
        }

        if (found)
        {
            gSelectedSegment = bestSelection;
            return true;
        }

        ClearSelectedSegment();
        return false;
    }

    bool TryGetSnappedVertex(const Vec2& requested, Vec2& snappedPoint)
    {
        bool found = false;
        double bestDistance = kSnapDistance;
        const Vec2 requestedScreen = WorldToScreen(requested);

        for (std::size_t polylineIndex = 0; polylineIndex < gPolylines.size(); ++polylineIndex)
        {
            const Polyline& polyline = gPolylines[polylineIndex];

            const double startDistance = Distance(requestedScreen, WorldToScreen(polyline.startPoint));
            if (startDistance <= bestDistance)
            {
                bestDistance = startDistance;
                snappedPoint = polyline.startPoint;
                found = true;
            }

            for (std::size_t segmentIndex = 0; segmentIndex < polyline.segments.size(); ++segmentIndex)
            {
                const double endDistance = Distance(requestedScreen, WorldToScreen(polyline.segments[segmentIndex].end));
                if (endDistance <= bestDistance)
                {
                    bestDistance = endDistance;
                    snappedPoint = polyline.segments[segmentIndex].end;
                    found = true;
                }
            }
        }

        return found;
    }

    bool IsPointNearSnapVertex(const Vec2& requested)
    {
        Vec2 snappedPoint;
        return TryGetSnappedVertex(requested, snappedPoint);
    }

    Vec2 GetSnappedVertexOrOriginal(const Vec2& requested)
    {
        Vec2 snappedPoint;
        return TryGetSnappedVertex(requested, snappedPoint) ? snappedPoint : requested;
    }

    void ClearDistanceInput()
    {
        gDistanceInputBuffer.clear();
        gHasDistanceDirection = false;
        gDistanceDirection = Vec2();
    }

    bool IsShiftPressed()
    {
        return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    Vec2 ApplyOrthogonalConstraint(const Vec2& anchor, const Vec2& requested)
    {
        const Vec2 delta = requested - anchor;
        if (std::fabs(delta.x) >= std::fabs(delta.y))
        {
            return Vec2(requested.x, anchor.y);
        }

        return Vec2(anchor.x, requested.y);
    }

    bool CanUseTypedDistance()
    {
        return gActivePolylineIndex >= 0 && !gHasPendingArcEnd;
    }

    bool CanUseTypedArcRadius()
    {
        return gActivePolylineIndex >= 0 && gInsertMode == InsertArc && gHasPendingArcEnd;
    }

    bool TryGetTypedDistance(double& distance)
    {
        if (gDistanceInputBuffer.empty())
        {
            return false;
        }

        std::string normalized = gDistanceInputBuffer;
        std::replace(normalized.begin(), normalized.end(), ',', '.');

        char* parseEnd = NULL;
        distance = std::strtod(normalized.c_str(), &parseEnd);
        return parseEnd != normalized.c_str() && *parseEnd == '\0' && distance > 0.0;
    }

    Vec2 GetOrthogonalPreviewEnd(const Vec2& anchor, const Vec2& requested)
    {
        return ApplyOrthogonalConstraint(anchor, GetSnappedVertexOrOriginal(requested));
    }

    void LockDistanceDirection(const Vec2& anchor, const Vec2& requested)
    {
        const Vec2 orthogonalEnd = GetOrthogonalPreviewEnd(anchor, requested);
        const Vec2 direction = Normalize(orthogonalEnd - anchor);
        if (Length(direction) > kEpsilon)
        {
            gHasDistanceDirection = true;
            gDistanceDirection = direction;
        }
    }

    bool TryGetTypedEndpoint(const Polyline& polyline, Vec2& typedEndPoint)
    {
        double typedDistance = 0.0;
        if (!gHasDistanceDirection || !TryGetTypedDistance(typedDistance))
        {
            return false;
        }

        typedEndPoint = GetCurrentEndPoint(polyline) + gDistanceDirection * typedDistance;
        return true;
    }

    Vec2 GetPreviewEndpoint(const Polyline& polyline, const Vec2& requested)
    {
        Vec2 typedEndPoint;
        if (TryGetTypedEndpoint(polyline, typedEndPoint))
        {
            return typedEndPoint;
        }

        Vec2 snappedPoint = GetSnappedVertexOrOriginal(requested);
        if (IsShiftPressed())
        {
            snappedPoint = ApplyOrthogonalConstraint(GetCurrentEndPoint(polyline), snappedPoint);
        }

        return snappedPoint;
    }

    void UpdateSnapHighlight()
    {
        gHasHighlightedSnapPoint = false;

        if (gInsertMode == InsertArc && gHasPendingArcEnd)
        {
            return;
        }

        Vec2 snappedPoint;
        if (TryGetSnappedVertex(gMousePosition, snappedPoint))
        {
            gHasHighlightedSnapPoint = true;
            gHighlightedSnapPoint = snappedPoint;
        }
    }

    Vec2 GetArcStartTangent(const Segment& segment)
    {
        const Vec2 radius = segment.start - segment.center;
        return Normalize(segment.clockwise ? PerpendicularRight(radius) : PerpendicularLeft(radius));
    }

    Vec2 GetArcEndTangent(const Segment& segment)
    {
        const Vec2 radius = segment.end - segment.center;
        return Normalize(segment.clockwise ? PerpendicularRight(radius) : PerpendicularLeft(radius));
    }

    bool TryGetOutgoingTangent(const Polyline& polyline, Vec2& tangent)
    {
        if (polyline.segments.empty())
        {
            return false;
        }

        const Segment& last = polyline.segments.back();
        if (last.type == SegmentLine)
        {
            tangent = Normalize(last.end - last.start);
            return Length(tangent) > kEpsilon;
        }

        tangent = GetArcEndTangent(last);
        return Length(tangent) > kEpsilon;
    }

    void UpdateWindowTitle()
    {
        std::ostringstream stream;
        stream << GetDisplayFileName() << " - miniCAD by SowaZ3    ";
        stream << (gInsertMode == InsertLine ? "Line" : "Arc");

        if (gActivePolylineIndex >= 0)
        {
            if (gHasPendingArcEnd)
            {
                stream << " | pick point on arc or type radius";
            }
            else if (gInsertMode == InsertArc)
            {
                stream << " | pick arc end";
            }
            else
            {
                stream << " | pick next point";
            }
        }
        else
        {
            stream << " | pick start point";
        }

        SetWindowTextA(gWindow, stream.str().c_str());
    }

    int GetEditablePolylineIndex()
    {
        if (gActivePolylineIndex >= 0)
        {
            return gActivePolylineIndex;
        }

        if (gPolylines.empty())
        {
            return -1;
        }

        return static_cast<int>(gPolylines.size()) - 1;
    }

    bool CanContinueLastPolyline()
    {
        return gActivePolylineIndex < 0 && !gPolylines.empty() && gPolylines.back().finished;
    }

    bool CanUndo()
    {
        return !gUndoStates.empty();
    }

    bool CanRedo()
    {
        return !gRedoStates.empty();
    }

    bool CanExportSvg()
    {
        for (std::size_t i = 0; i < gPolylines.size(); ++i)
        {
            if (!gPolylines[i].segments.empty())
            {
                return true;
            }
        }

        return false;
    }

    void FitViewToBounds(const Bounds& bounds)
    {
        if (!bounds.initialized || gClientWidth <= 0 || gClientHeight <= 0)
        {
            gViewOrigin = Vec2(0.0, 0.0);
            gZoom = 1.0;
            return;
        }

        const double margin = 20.0;
        const double width = std::max(1.0, (bounds.max.x - bounds.min.x) + 2.0 * margin);
        const double height = std::max(1.0, (bounds.max.y - bounds.min.y) + 2.0 * margin);
        const double zoomX = static_cast<double>(gClientWidth) / width;
        const double zoomY = static_cast<double>(gClientHeight) / height;

        gZoom = std::max(kMinZoom, std::min(kMaxZoom, std::min(zoomX, zoomY)));

        const double visibleWidth = static_cast<double>(gClientWidth) / gZoom;
        const double visibleHeight = static_cast<double>(gClientHeight) / gZoom;
        const double contentMinX = bounds.min.x - margin;
        const double contentMinY = bounds.min.y - margin;

        gViewOrigin.x = contentMinX - (visibleWidth - width) * 0.5;
        gViewOrigin.y = contentMinY - (visibleHeight - height) * 0.5;
    }

    void InvalidateEditor()
    {
        InvalidateRect(gWindow, NULL, FALSE);
        UpdateWindowTitle();
        UpdateMainMenuState();
    }

    EditorState CaptureEditorState()
    {
        EditorState state;
        state.polylines = gPolylines;
        state.activePolylineIndex = gActivePolylineIndex;
        state.hasPendingArcEnd = gHasPendingArcEnd;
        state.pendingArcEnd = gPendingArcEnd;
        state.distanceInputBuffer = gDistanceInputBuffer;
        state.hasDistanceDirection = gHasDistanceDirection;
        state.distanceDirection = gDistanceDirection;
        return state;
    }

    void RestoreEditorState(const EditorState& state)
    {
        gPolylines = state.polylines;
        gActivePolylineIndex = state.activePolylineIndex;
        gHasPendingArcEnd = state.hasPendingArcEnd;
        gPendingArcEnd = state.pendingArcEnd;
        gDistanceInputBuffer = state.distanceInputBuffer;
        gHasDistanceDirection = state.hasDistanceDirection;
        gDistanceDirection = state.distanceDirection;
        ClearSelectedSegment();
        UpdateSnapHighlight();
    }

    void PushUndoState()
    {
        gUndoStates.push_back(CaptureEditorState());
        gRedoStates.clear();
    }

    Vec2 SnapPointForLine(const Polyline& polyline, const Vec2& requested)
    {
        return GetPreviewEndpoint(polyline, requested);
    }

    bool IsAngleOnCounterClockwiseArc(double startAngle, double endAngle, double targetAngle)
    {
        const double fullTurn = 2.0 * kPi;
        while (startAngle < 0.0) startAngle += fullTurn;
        while (startAngle >= fullTurn) startAngle -= fullTurn;
        while (endAngle < 0.0) endAngle += fullTurn;
        while (endAngle >= fullTurn) endAngle -= fullTurn;
        while (targetAngle < 0.0) targetAngle += fullTurn;
        while (targetAngle >= fullTurn) targetAngle -= fullTurn;

        while (endAngle < startAngle)
        {
            endAngle += fullTurn;
        }

        while (targetAngle < startAngle)
        {
            targetAngle += fullTurn;
        }

        return targetAngle <= endAngle;
    }

    bool BuildArcSegment(const Polyline& polyline, const Vec2& requestedEnd, const Vec2& pointOnArc, Segment& segment)
    {
        const Vec2 start = GetCurrentEndPoint(polyline);
        if (Distance(start, requestedEnd) < 2.0)
        {
            return false;
        }

        if (Distance(start, pointOnArc) < 2.0 || Distance(requestedEnd, pointOnArc) < 2.0)
        {
            return false;
        }

        const double determinant = 2.0 * (
            start.x * (requestedEnd.y - pointOnArc.y) +
            requestedEnd.x * (pointOnArc.y - start.y) +
            pointOnArc.x * (start.y - requestedEnd.y)
        );

        if (std::fabs(determinant) <= 1e-9)
        {
            return false;
        }

        const double startSquared = start.x * start.x + start.y * start.y;
        const double endSquared = requestedEnd.x * requestedEnd.x + requestedEnd.y * requestedEnd.y;
        const double arcSquared = pointOnArc.x * pointOnArc.x + pointOnArc.y * pointOnArc.y;

        const Vec2 center(
            (startSquared * (requestedEnd.y - pointOnArc.y) +
             endSquared * (pointOnArc.y - start.y) +
             arcSquared * (start.y - requestedEnd.y)) / determinant,
            (startSquared * (pointOnArc.x - requestedEnd.x) +
             endSquared * (start.x - pointOnArc.x) +
             arcSquared * (requestedEnd.x - start.x)) / determinant
        );

        segment.type = SegmentArc;
        segment.start = start;
        segment.end = requestedEnd;
        segment.center = center;

        const double startAngle = std::atan2(start.y - center.y, start.x - center.x);
        const double endAngle = std::atan2(requestedEnd.y - center.y, requestedEnd.x - center.x);
        const double throughAngle = std::atan2(pointOnArc.y - center.y, pointOnArc.x - center.x);

        segment.clockwise = !IsAngleOnCounterClockwiseArc(startAngle, endAngle, throughAngle);
        return true;
    }

    bool BuildArcSegmentFromRadius(const Polyline& polyline, const Vec2& requestedEnd, double radius, const Vec2& sidePoint, Segment& segment)
    {
        const Vec2 start = GetCurrentEndPoint(polyline);
        const Vec2 chord = requestedEnd - start;
        const double chordLength = Length(chord);
        if (chordLength < 2.0)
        {
            return false;
        }

        const double halfChord = chordLength * 0.5;
        if (radius + kEpsilon < halfChord)
        {
            return false;
        }

        const Vec2 midpoint = (start + requestedEnd) * 0.5;
        const Vec2 leftNormal = Normalize(PerpendicularLeft(chord));
        if (Length(leftNormal) <= kEpsilon)
        {
            return false;
        }

        double side = Cross(chord, sidePoint - midpoint);
        if (std::fabs(side) <= kEpsilon)
        {
            side = 1.0;
        }

        const Vec2 bulgeNormal = side >= 0.0 ? leftNormal : leftNormal * -1.0;
        const double centerOffset = std::sqrt(std::max(0.0, radius * radius - halfChord * halfChord));
        const double sagitta = radius - centerOffset;
        const Vec2 pointOnArc = midpoint + bulgeNormal * sagitta;

        return BuildArcSegment(polyline, requestedEnd, pointOnArc, segment);
    }

    void FinishCurrentPolyline()
    {
        if (gActivePolylineIndex < 0)
        {
            return;
        }

        PushUndoState();
        gPolylines[gActivePolylineIndex].finished = true;
        gActivePolylineIndex = -1;
        gHasPendingArcEnd = false;
        ClearDistanceInput();
        ClearSelectedSegment();
        InvalidateEditor();
    }

    void ContinueLastPolyline()
    {
        if (!CanContinueLastPolyline())
        {
            return;
        }

        PushUndoState();
        gActivePolylineIndex = static_cast<int>(gPolylines.size()) - 1;
        gPolylines[gActivePolylineIndex].finished = false;
        gHasPendingArcEnd = false;
        ClearDistanceInput();
        ClearSelectedSegment();
        InvalidateEditor();
    }

    void UndoLastStep()
    {
        if (!CanUndo())
        {
            return;
        }

        gRedoStates.push_back(CaptureEditorState());
        const EditorState state = gUndoStates.back();
        gUndoStates.pop_back();
        RestoreEditorState(state);
        InvalidateEditor();
    }

    void RedoLastStep()
    {
        if (!CanRedo())
        {
            return;
        }

        gUndoStates.push_back(CaptureEditorState());
        const EditorState state = gRedoStates.back();
        gRedoStates.pop_back();
        RestoreEditorState(state);
        InvalidateEditor();
    }

    void StartNewPolyline(const Vec2& point)
    {
        PushUndoState();
        const Vec2 snappedStartPoint = GetSnappedVertexOrOriginal(point);
        Polyline polyline;
        polyline.startPoint = snappedStartPoint;
        polyline.finished = false;
        gPolylines.push_back(polyline);
        gActivePolylineIndex = static_cast<int>(gPolylines.size()) - 1;
        gHasPendingArcEnd = false;
        ClearDistanceInput();
        ClearSelectedSegment();
        UpdateSnapHighlight();
        InvalidateEditor();
    }

    void AddLinePoint(const Vec2& point)
    {
        if (gActivePolylineIndex < 0)
        {
            StartNewPolyline(point);
            return;
        }

        Polyline& polyline = gPolylines[gActivePolylineIndex];
        Segment segment;
        segment.type = SegmentLine;
        segment.start = GetCurrentEndPoint(polyline);
        segment.end = SnapPointForLine(polyline, point);
        segment.center = Vec2();
        segment.clockwise = false;

        if (Distance(segment.start, segment.end) < 2.0)
        {
            return;
        }

        PushUndoState();
        polyline.segments.push_back(segment);
        gHasPendingArcEnd = false;
        ClearDistanceInput();
        UpdateSnapHighlight();
        InvalidateEditor();
    }

    void AddArcSegment(const Segment& segment)
    {
        if (gActivePolylineIndex < 0)
        {
            return;
        }

        PushUndoState();
        gPolylines[gActivePolylineIndex].segments.push_back(segment);
        gHasPendingArcEnd = false;
        ClearDistanceInput();
        UpdateSnapHighlight();
        InvalidateEditor();
    }

    void DeleteSelectedSegment()
    {
        if (!HasSelectedSegment() || gActivePolylineIndex >= 0)
        {
            return;
        }

        PushUndoState();

        const int polylineIndex = gSelectedSegment.polylineIndex;
        const int segmentIndex = gSelectedSegment.segmentIndex;
        Polyline& polyline = gPolylines[polylineIndex];

        if (polyline.segments.size() == 1)
        {
            gPolylines.erase(gPolylines.begin() + polylineIndex);
        }
        else if (segmentIndex == 0)
        {
            polyline.startPoint = polyline.segments[0].end;
            polyline.segments.erase(polyline.segments.begin());
        }
        else if (segmentIndex == static_cast<int>(polyline.segments.size()) - 1)
        {
            polyline.segments.erase(polyline.segments.begin() + segmentIndex);
        }
        else
        {
            Polyline trailingPart;
            trailingPart.startPoint = polyline.segments[segmentIndex].end;
            trailingPart.finished = polyline.finished;
            trailingPart.segments.assign(
                polyline.segments.begin() + segmentIndex + 1,
                polyline.segments.end()
            );

            polyline.segments.erase(
                polyline.segments.begin() + segmentIndex,
                polyline.segments.end()
            );

            gPolylines.insert(gPolylines.begin() + polylineIndex + 1, trailingPart);
        }

        ClearSelectedSegment();
        UpdateSnapHighlight();
        InvalidateEditor();
    }

    void SetPendingArcEnd(const Vec2& point)
    {
        if (gActivePolylineIndex < 0 || gHasPendingArcEnd)
        {
            return;
        }

        Polyline& polyline = gPolylines[gActivePolylineIndex];
        if (Distance(point, GetCurrentEndPoint(polyline)) < 2.0)
        {
            return;
        }

        PushUndoState();
        gPendingArcEnd = point;
        gHasPendingArcEnd = true;
        ClearDistanceInput();
        UpdateSnapHighlight();
        InvalidateEditor();
    }

    void HandleArcClick(const Vec2& point)
    {
        if (gActivePolylineIndex < 0)
        {
            StartNewPolyline(point);
            return;
        }

        Polyline& polyline = gPolylines[gActivePolylineIndex];
        if (!gHasPendingArcEnd)
        {
            SetPendingArcEnd(GetPreviewEndpoint(polyline, point));
            return;
        }

        Segment segment;
        if (!BuildArcSegment(polyline, gPendingArcEnd, point, segment))
        {
            return;
        }

        AddArcSegment(segment);
    }

    double NormalizeAngle(double angle)
    {
        while (angle < 0.0)
        {
            angle += 2.0 * kPi;
        }

        while (angle >= 2.0 * kPi)
        {
            angle -= 2.0 * kPi;
        }

        return angle;
    }

    double GetArcSweep(const Segment& segment)
    {
        const double startAngle = NormalizeAngle(std::atan2(segment.start.y - segment.center.y, segment.start.x - segment.center.x));
        double endAngle = NormalizeAngle(std::atan2(segment.end.y - segment.center.y, segment.end.x - segment.center.x));

        if (segment.clockwise)
        {
            while (endAngle >= startAngle)
            {
                endAngle -= 2.0 * kPi;
            }
        }
        else
        {
            while (endAngle <= startAngle)
            {
                endAngle += 2.0 * kPi;
            }
        }

        return endAngle - startAngle;
    }

    void DrawCircle(const Vec2& center, double radius)
    {
        glBegin(GL_TRIANGLE_FAN);
        glVertex2d(center.x, center.y);

        for (int i = 0; i <= 20; ++i)
        {
            const double angle = (2.0 * kPi * static_cast<double>(i)) / 20.0;
            glVertex2d(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
        }

        glEnd();
    }

    void DrawArcGeometry(const Segment& segment)
    {
        const Vec2 radiusVector = segment.start - segment.center;
        const double radius = Length(radiusVector);
        const double startAngle = std::atan2(radiusVector.y, radiusVector.x);
        const double sweep = GetArcSweep(segment);
        const int steps = std::max(24, static_cast<int>(std::fabs(sweep) * radius / 8.0));

        glBegin(GL_LINE_STRIP);
        for (int i = 0; i <= steps; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(steps);
            const double angle = startAngle + sweep * t;
            glVertex2d(segment.center.x + std::cos(angle) * radius, segment.center.y + std::sin(angle) * radius);
        }
        glEnd();
    }

    void DrawSegment(const Segment& segment)
    {
        if (segment.type == SegmentLine)
        {
            glBegin(GL_LINES);
            glVertex2d(segment.start.x, segment.start.y);
            glVertex2d(segment.end.x, segment.end.y);
            glEnd();
            return;
        }

        DrawArcGeometry(segment);
    }

    void DrawSelectedSegmentOverlay()
    {
        if (!HasSelectedSegment())
        {
            return;
        }

        const Segment& segment = gPolylines[gSelectedSegment.polylineIndex].segments[gSelectedSegment.segmentIndex];
        glLineWidth(2.6f);
        glColor3f(1.0f, 0.62f, 0.22f);
        DrawSegment(segment);
        glColor3f(1.0f, 0.72f, 0.32f);
        DrawCircle(segment.start, GetHandleRadiusWorld() * 1.4);
        DrawCircle(segment.end, GetHandleRadiusWorld() * 1.4);
        glLineWidth(1.2f);
    }

    void DrawPolyline(const Polyline& polyline)
    {
        glColor3f(0.88f, 0.90f, 0.93f);
        for (std::size_t i = 0; i < polyline.segments.size(); ++i)
        {
            DrawSegment(polyline.segments[i]);
        }

        const double handleRadius = GetHandleRadiusWorld();
        glColor3f(0.70f, 0.74f, 0.79f);
        DrawCircle(polyline.startPoint, handleRadius);
        for (std::size_t i = 0; i < polyline.segments.size(); ++i)
        {
            DrawCircle(polyline.segments[i].end, handleRadius);
        }
    }

    void DrawSnapHighlight()
    {
        if (!gHasHighlightedSnapPoint)
        {
            return;
        }

        const double outerRadius = GetHandleRadiusWorld() * 2.1;
        const double innerRadius = GetHandleRadiusWorld() * 1.1;

        glColor3f(0.45f, 0.85f, 1.0f);
        DrawCircle(gHighlightedSnapPoint, outerRadius);
        glColor3f(0.16f, 0.18f, 0.20f);
        DrawCircle(gHighlightedSnapPoint, innerRadius);
    }

    void DrawPreview()
    {
        if (gActivePolylineIndex < 0)
        {
            return;
        }

        const Polyline& polyline = gPolylines[gActivePolylineIndex];
        const Vec2 anchor = GetCurrentEndPoint(polyline);

        glLineWidth(1.0f);
        glColor3f(0.62f, 0.66f, 0.72f);

        if (gInsertMode == InsertLine)
        {
            const Vec2 previewEnd = SnapPointForLine(polyline, gMousePosition);
            glBegin(GL_LINES);
            glVertex2d(anchor.x, anchor.y);
            glVertex2d(previewEnd.x, previewEnd.y);
            glEnd();
            return;
        }

        if (!gHasPendingArcEnd)
        {
            const Vec2 previewEnd = GetPreviewEndpoint(polyline, gMousePosition);
            glBegin(GL_LINES);
            glVertex2d(anchor.x, anchor.y);
            glVertex2d(previewEnd.x, previewEnd.y);
            glEnd();

            glColor3f(0.45f, 0.85f, 1.0f);
            DrawCircle(previewEnd, GetHandleRadiusWorld());
            return;
        }

        Segment previewArc;
        bool hasPreviewArc = false;
        if (CanUseTypedArcRadius())
        {
            double radius = 0.0;
            if (TryGetTypedDistance(radius))
            {
                hasPreviewArc = BuildArcSegmentFromRadius(polyline, gPendingArcEnd, radius, gMousePosition, previewArc);
            }
        }

        if (!hasPreviewArc)
        {
            hasPreviewArc = BuildArcSegment(polyline, gPendingArcEnd, gMousePosition, previewArc);
        }

        if (hasPreviewArc)
        {
            DrawArcGeometry(previewArc);
        }
        else
        {
            glBegin(GL_LINES);
            glVertex2d(anchor.x, anchor.y);
            glVertex2d(gPendingArcEnd.x, gPendingArcEnd.y);
            glEnd();
        }

        glColor3f(0.45f, 0.85f, 1.0f);
        DrawCircle(gPendingArcEnd, GetHandleRadiusWorld());
        glBegin(GL_LINES);
        glVertex2d(anchor.x, anchor.y);
        glVertex2d(gPendingArcEnd.x, gPendingArcEnd.y);
        glEnd();
    }

    void AppendSegmentBounds(Bounds& bounds, const Segment& segment)
    {
        ExpandBounds(bounds, segment.start);
        ExpandBounds(bounds, segment.end);

        if (segment.type == SegmentLine)
        {
            return;
        }

        const Vec2 radiusVector = segment.start - segment.center;
        const double radius = Length(radiusVector);
        const double startAngle = std::atan2(radiusVector.y, radiusVector.x);
        const double sweep = GetArcSweep(segment);
        const int steps = std::max(24, static_cast<int>(std::fabs(sweep) * radius / 8.0));

        for (int i = 1; i < steps; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(steps);
            const double angle = startAngle + sweep * t;
            ExpandBounds(bounds, Vec2(
                segment.center.x + std::cos(angle) * radius,
                segment.center.y + std::sin(angle) * radius
            ));
        }
    }

    Bounds GetDrawingBounds()
    {
        Bounds bounds;
        for (std::size_t polylineIndex = 0; polylineIndex < gPolylines.size(); ++polylineIndex)
        {
            const Polyline& polyline = gPolylines[polylineIndex];
            for (std::size_t segmentIndex = 0; segmentIndex < polyline.segments.size(); ++segmentIndex)
            {
                AppendSegmentBounds(bounds, polyline.segments[segmentIndex]);
            }
        }

        return bounds;
    }

    bool TryParseSvgAttribute(const std::string& line, const char* attributeName, double& value)
    {
        const std::string pattern = std::string(attributeName) + "=\"";
        const std::size_t start = line.find(pattern);
        if (start == std::string::npos)
        {
            return false;
        }

        const std::size_t valueStart = start + pattern.size();
        const std::size_t valueEnd = line.find('"', valueStart);
        if (valueEnd == std::string::npos)
        {
            return false;
        }

        const std::string token = line.substr(valueStart, valueEnd - valueStart);
        char* parseEnd = NULL;
        value = std::strtod(token.c_str(), &parseEnd);
        return parseEnd != token.c_str() && *parseEnd == '\0';
    }

    bool BuildArcSegmentFromSvg(
        const Vec2& start,
        const Vec2& end,
        double radius,
        int largeArcFlag,
        int sweepFlag,
        Segment& segment)
    {
        const Vec2 chord = end - start;
        const double chordLength = Length(chord);
        if (chordLength <= kEpsilon || radius + kEpsilon < chordLength * 0.5)
        {
            return false;
        }

        const Vec2 midpoint = (start + end) * 0.5;
        const Vec2 leftNormal = Normalize(PerpendicularLeft(chord));
        if (Length(leftNormal) <= kEpsilon)
        {
            return false;
        }

        const double halfChord = chordLength * 0.5;
        const double centerOffset = std::sqrt(std::max(0.0, radius * radius - halfChord * halfChord));

        Vec2 candidateCenters[2];
        int candidateCount = 1;
        candidateCenters[0] = midpoint + leftNormal * centerOffset;
        if (centerOffset > kEpsilon)
        {
            candidateCenters[1] = midpoint - leftNormal * centerOffset;
            candidateCount = 2;
        }

        for (int centerIndex = 0; centerIndex < candidateCount; ++centerIndex)
        {
            for (int clockwiseIndex = 0; clockwiseIndex < 2; ++clockwiseIndex)
            {
                Segment candidate;
                candidate.type = SegmentArc;
                candidate.start = start;
                candidate.end = end;
                candidate.center = candidateCenters[centerIndex];
                candidate.clockwise = clockwiseIndex != 0;

                const double sweep = GetArcSweep(candidate);
                const bool matchesSweep = (sweep >= 0.0) == (sweepFlag != 0);
                if (!matchesSweep)
                {
                    continue;
                }

                const double absSweep = std::fabs(sweep);
                const bool nearSemicircle = std::fabs(absSweep - kPi) <= 1e-4;
                const bool matchesLargeArc = nearSemicircle ||
                    ((absSweep > kPi) == (largeArcFlag != 0));

                if (matchesLargeArc)
                {
                    segment = candidate;
                    return true;
                }
            }
        }

        return false;
    }

    bool LoadSvgSegments(const std::string& filePath, std::vector<Segment>& segments)
    {
        std::ifstream input(filePath.c_str());
        if (!input)
        {
            return false;
        }

        std::string line;
        while (std::getline(input, line))
        {
            if (line.find("<line ") != std::string::npos)
            {
                double x1 = 0.0;
                double y1 = 0.0;
                double x2 = 0.0;
                double y2 = 0.0;
                if (!TryParseSvgAttribute(line, "x1", x1) ||
                    !TryParseSvgAttribute(line, "y1", y1) ||
                    !TryParseSvgAttribute(line, "x2", x2) ||
                    !TryParseSvgAttribute(line, "y2", y2))
                {
                    continue;
                }

                Segment segment;
                segment.type = SegmentLine;
                segment.start = Vec2(x1, y1);
                segment.end = Vec2(x2, y2);
                segments.push_back(segment);
                continue;
            }

            const std::size_t dAttribute = line.find("d=\"");
            if (line.find("<path ") != std::string::npos && dAttribute != std::string::npos)
            {
                const std::size_t dStart = dAttribute + 3;
                const std::size_t dEnd = line.find('"', dStart);
                if (dEnd == std::string::npos)
                {
                    continue;
                }

                const std::string pathData = line.substr(dStart, dEnd - dStart);
                double startX = 0.0;
                double startY = 0.0;
                double radiusX = 0.0;
                double radiusY = 0.0;
                double axisRotation = 0.0;
                int largeArcFlag = 0;
                int sweepFlag = 0;
                double endX = 0.0;
                double endY = 0.0;

                if (std::sscanf(
                        pathData.c_str(),
                        "M %lf %lf A %lf %lf %lf %d %d %lf %lf",
                        &startX,
                        &startY,
                        &radiusX,
                        &radiusY,
                        &axisRotation,
                        &largeArcFlag,
                        &sweepFlag,
                        &endX,
                        &endY) != 9)
                {
                    continue;
                }

                if (std::fabs(radiusX - radiusY) > 1e-3 || std::fabs(axisRotation) > 1e-3)
                {
                    continue;
                }

                Segment segment;
                if (BuildArcSegmentFromSvg(
                        Vec2(startX, startY),
                        Vec2(endX, endY),
                        radiusX,
                        largeArcFlag,
                        sweepFlag,
                        segment))
                {
                    segments.push_back(segment);
                }
            }
        }

        return !segments.empty();
    }

    std::vector<Polyline> BuildPolylinesFromSegments(const std::vector<Segment>& segments)
    {
        std::vector<Polyline> polylines;
        const double chainTolerance = 1e-3;

        for (std::size_t segmentIndex = 0; segmentIndex < segments.size(); ++segmentIndex)
        {
            const Segment& segment = segments[segmentIndex];
            if (!polylines.empty() &&
                Distance(GetCurrentEndPoint(polylines.back()), segment.start) <= chainTolerance)
            {
                polylines.back().segments.push_back(segment);
                continue;
            }

            Polyline polyline;
            polyline.startPoint = segment.start;
            polyline.finished = true;
            polyline.segments.push_back(segment);
            polylines.push_back(polyline);
        }

        return polylines;
    }

    void WriteSvgSegment(std::ofstream& output, const Segment& segment)
    {
        if (segment.type == SegmentLine)
        {
            output
                << "<line x1=\"" << FormatSvgNumber(segment.start.x)
                << "\" y1=\"" << FormatSvgNumber(segment.start.y)
                << "\" x2=\"" << FormatSvgNumber(segment.end.x)
                << "\" y2=\"" << FormatSvgNumber(segment.end.y)
                << "\" stroke=\"#000000\" stroke-width=\"1.5\" fill=\"none\" stroke-linecap=\"round\" />\n";
            return;
        }

        const double radius = Length(segment.start - segment.center);
        const double sweep = GetArcSweep(segment);
        const int largeArcFlag = std::fabs(sweep) > kPi ? 1 : 0;
        const int sweepFlag = sweep >= 0.0 ? 1 : 0;

        output
            << "<path d=\"M " << FormatSvgNumber(segment.start.x) << " " << FormatSvgNumber(segment.start.y)
            << " A " << FormatSvgNumber(radius) << " " << FormatSvgNumber(radius)
            << " 0 " << largeArcFlag << " " << sweepFlag << " "
            << FormatSvgNumber(segment.end.x) << " " << FormatSvgNumber(segment.end.y)
            << "\" stroke=\"#000000\" stroke-width=\"1.5\" fill=\"none\" stroke-linecap=\"round\" />\n";
    }

    bool ExportSvgToFile(const std::string& filePath)
    {
        Bounds bounds = GetDrawingBounds();
        if (!bounds.initialized)
        {
            return false;
        }

        const double margin = 20.0;
        const double minX = bounds.min.x - margin;
        const double minY = bounds.min.y - margin;
        const double width = std::max(1.0, (bounds.max.x - bounds.min.x) + 2.0 * margin);
        const double height = std::max(1.0, (bounds.max.y - bounds.min.y) + 2.0 * margin);

        std::ofstream output(filePath.c_str(), std::ios::out | std::ios::trunc);
        if (!output)
        {
            return false;
        }

        output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        output
            << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" viewBox=\""
            << FormatSvgNumber(minX) << " "
            << FormatSvgNumber(minY) << " "
            << FormatSvgNumber(width) << " "
            << FormatSvgNumber(height) << "\">\n";
        output
            << "<rect x=\"" << FormatSvgNumber(minX)
            << "\" y=\"" << FormatSvgNumber(minY)
            << "\" width=\"" << FormatSvgNumber(width)
            << "\" height=\"" << FormatSvgNumber(height)
            << "\" fill=\"#ffffff\" />\n";

        for (std::size_t polylineIndex = 0; polylineIndex < gPolylines.size(); ++polylineIndex)
        {
            const Polyline& polyline = gPolylines[polylineIndex];
            for (std::size_t segmentIndex = 0; segmentIndex < polyline.segments.size(); ++segmentIndex)
            {
                WriteSvgSegment(output, polyline.segments[segmentIndex]);
            }
        }

        output << "</svg>\n";
        return output.good();
    }

    std::string PromptForSvgPath(bool saveDialog, const char* defaultName)
    {
        char filePath[MAX_PATH] = { 0 };
        if (defaultName && *defaultName)
        {
            lstrcpynA(filePath, defaultName, MAX_PATH);
        }

        OPENFILENAMEA dialog;
        ZeroMemory(&dialog, sizeof(dialog));
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = gWindow;
        dialog.lpstrFilter = "SVG files (*.svg)\0*.svg\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = filePath;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrDefExt = "svg";
        dialog.Flags = OFN_PATHMUSTEXIST | (saveDialog ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);

        const BOOL accepted = saveDialog ? GetSaveFileNameA(&dialog) : GetOpenFileNameA(&dialog);
        return accepted ? std::string(filePath) : std::string();
    }

    void NewDocument()
    {
        ResetEditorDocumentState();
        gCurrentFilePath.clear();
        gViewOrigin = Vec2(0.0, 0.0);
        gZoom = 1.0;
        InvalidateEditor();
    }

    bool SaveDocumentToPath(const std::string& filePath)
    {
        if (!ExportSvgToFile(filePath))
        {
            MessageBoxA(gWindow, "SVG save failed.", "Save SVG", MB_OK | MB_ICONERROR);
            return false;
        }

        gCurrentFilePath = filePath;
        InvalidateEditor();
        return true;
    }

    void SaveDocumentAs()
    {
        const std::string suggestedName = gCurrentFilePath.empty() ? "drawing.svg" : gCurrentFilePath;
        const std::string filePath = PromptForSvgPath(true, suggestedName.c_str());
        if (filePath.empty())
        {
            return;
        }

        SaveDocumentToPath(filePath);
    }

    void SaveDocument()
    {
        if (gCurrentFilePath.empty())
        {
            SaveDocumentAs();
            return;
        }

        SaveDocumentToPath(gCurrentFilePath);
    }

    void OpenDocument()
    {
        const std::string filePath = PromptForSvgPath(false, "");
        if (filePath.empty())
        {
            return;
        }

        std::vector<Segment> segments;
        if (!LoadSvgSegments(filePath, segments))
        {
            MessageBoxA(gWindow, "SVG file could not be loaded or contains unsupported geometry.", "Open SVG", MB_OK | MB_ICONERROR);
            return;
        }

        ResetEditorDocumentState();
        gPolylines = BuildPolylinesFromSegments(segments);
        gCurrentFilePath = filePath;
        FitViewToBounds(GetDrawingBounds());
        InvalidateEditor();
    }

    void ExportSvg()
    {
        if (!CanExportSvg())
        {
            MessageBoxA(gWindow, "There is no geometry to export.", "Export SVG", MB_OK | MB_ICONINFORMATION);
            return;
        }

        const std::string filePath = PromptForSvgPath(true, "drawing.svg");
        if (filePath.empty())
        {
            return;
        }

        if (!ExportSvgToFile(filePath))
        {
            MessageBoxA(gWindow, "SVG export failed.", "Export SVG", MB_OK | MB_ICONERROR);
            return;
        }

        MessageBoxA(gWindow, "SVG file saved successfully.", "Export SVG", MB_OK | MB_ICONINFORMATION);
    }

    std::string BuildHudStatusText()
    {
        if (gActivePolylineIndex < 0)
        {
            if (!HasSelectedSegment())
            {
                return "";
            }

            const Segment& segment = gPolylines[gSelectedSegment.polylineIndex].segments[gSelectedSegment.segmentIndex];
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(2);
            stream << (segment.type == SegmentLine ? "Line" : "Arc");
            stream << "  Start: " << FormatPointForHud(segment.start);
            stream << "  End: " << FormatPointForHud(segment.end);

            if (segment.type == SegmentArc)
            {
                stream << "  Radius: " << Length(segment.start - segment.center);
            }

            return stream.str();
        }

        std::ostringstream stream;
        stream << std::fixed << std::setprecision(2);

        const Polyline& polyline = gPolylines[gActivePolylineIndex];
        const Vec2 anchor = GetCurrentEndPoint(polyline);

        if (gInsertMode == InsertLine)
        {
            const Vec2 previewEnd = SnapPointForLine(polyline, gMousePosition);
            stream << "Distance: " << Distance(anchor, previewEnd);

            if (IsShiftPressed())
            {
                stream << "  Ortho";
            }

            if (!gDistanceInputBuffer.empty())
            {
                stream << "  Input: " << gDistanceInputBuffer;
            }
            else if (IsShiftPressed())
            {
                stream << "  Type length";
            }
        }
        else if (!gHasPendingArcEnd)
        {
            const Vec2 previewEnd = GetPreviewEndpoint(polyline, gMousePosition);
            stream << "Distance: " << Distance(anchor, previewEnd);

            if (IsShiftPressed())
            {
                stream << "  Ortho";
            }

            if (!gDistanceInputBuffer.empty())
            {
                stream << "  Input: " << gDistanceInputBuffer;
            }
            else if (IsShiftPressed())
            {
                stream << "  Type length";
            }
        }
        else
        {
            stream << "Chord: " << Distance(anchor, gPendingArcEnd);

            if (!gDistanceInputBuffer.empty())
            {
                stream << "  Radius: " << gDistanceInputBuffer;
            }
            else
            {
                stream << "  Pick point on arc or type radius";
            }
        }

        return stream.str();
    }

    bool CreateHudFont()
    {
        if (gHudFontBase != 0)
        {
            return true;
        }

        gHudFontBase = glGenLists(96);
        if (gHudFontBase == 0)
        {
            return false;
        }

        gHudFont = CreateFontA(
            -16,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            ANSI_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY,
            FF_DONTCARE,
            "Consolas"
        );

        if (!gHudFont)
        {
            glDeleteLists(gHudFontBase, 96);
            gHudFontBase = 0;
            return false;
        }

        HFONT oldFont = static_cast<HFONT>(SelectObject(gWindowDC, gHudFont));
        const BOOL result = wglUseFontBitmapsA(gWindowDC, 32, 96, gHudFontBase);
        SelectObject(gWindowDC, oldFont);

        if (!result)
        {
            DeleteObject(gHudFont);
            gHudFont = NULL;
            glDeleteLists(gHudFontBase, 96);
            gHudFontBase = 0;
            return false;
        }

        return true;
    }

    void DestroyHudFont()
    {
        if (gHudFontBase != 0)
        {
            glDeleteLists(gHudFontBase, 96);
            gHudFontBase = 0;
        }

        if (gHudFont)
        {
            DeleteObject(gHudFont);
            gHudFont = NULL;
        }
    }

    void DrawHudText(double x, double y, const std::string& text)
    {
        if (text.empty() || gHudFontBase == 0)
        {
            return;
        }

        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0.0, static_cast<double>(gClientWidth), static_cast<double>(gClientHeight), 0.0, -1.0, 1.0);

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();

        glPushAttrib(GL_LIST_BIT | GL_CURRENT_BIT | GL_ENABLE_BIT);
        glDisable(GL_TEXTURE_2D);
        glColor3f(0.85f, 0.90f, 0.95f);
        glListBase(gHudFontBase - 32);
        glRasterPos2d(x, y);
        glCallLists(static_cast<GLsizei>(text.size()), GL_UNSIGNED_BYTE, text.c_str());
        glPopAttrib();

        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
    }

    void DrawOverlayText(HDC hdc)
    {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(60, 60, 60));

        std::ostringstream stream;
        stream << "LMB: draw  |  RMB: menu  |  Mode: " << (gInsertMode == InsertLine ? "Line" : "Arc");
        if (gInsertMode == InsertArc)
        {
            stream << " (end -> point on arc)";
        }

        const std::string line1 = stream.str();
        const char* line2 = "Enter: Finish  |  Esc: Continue  |  Ctrl+Z: Undo";
        const char* line3 = "Arc joins are constrained to stay tangent to the previous segment.";

        TextOutA(hdc, 12, 10, line1.c_str(), static_cast<int>(line1.size()));
        TextOutA(hdc, 12, 28, line2, lstrlenA(line2));
        TextOutA(hdc, 12, 46, line3, lstrlenA(line3));
    }

    void RenderScene(HDC hdc)
    {
        if (!gGlContext)
        {
            return;
        }

        wglMakeCurrent(gWindowDC, gGlContext);

        glViewport(0, 0, gClientWidth, gClientHeight);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(
            gViewOrigin.x,
            gViewOrigin.x + static_cast<double>(gClientWidth) / gZoom,
            gViewOrigin.y + static_cast<double>(gClientHeight) / gZoom,
            gViewOrigin.y,
            -1.0,
            1.0
        );

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glClearColor(0.16f, 0.18f, 0.20f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_LINE_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
        glLineWidth(1.2f);

        for (std::size_t i = 0; i < gPolylines.size(); ++i)
        {
            DrawPolyline(gPolylines[i]);
        }

        DrawSelectedSegmentOverlay();
        DrawSnapHighlight();
        DrawPreview();
        DrawHudText(12.0, static_cast<double>(gClientHeight) - 16.0, BuildHudStatusText());

        SwapBuffers(gWindowDC);
        //DrawOverlayText(hdc);
    }

    bool CreateOpenGlContext(HWND hwnd)
    {
        gWindowDC = GetDC(hwnd);
        if (!gWindowDC)
        {
            return false;
        }

        PIXELFORMATDESCRIPTOR pfd;
        ZeroMemory(&pfd, sizeof(pfd));
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;
        pfd.cDepthBits = 16;
        pfd.iLayerType = PFD_MAIN_PLANE;

        const int pixelFormat = ChoosePixelFormat(gWindowDC, &pfd);
        if (pixelFormat == 0)
        {
            return false;
        }

        if (!SetPixelFormat(gWindowDC, pixelFormat, &pfd))
        {
            return false;
        }

        gGlContext = wglCreateContext(gWindowDC);
        if (!gGlContext)
        {
            return false;
        }

        if (!wglMakeCurrent(gWindowDC, gGlContext))
        {
            return false;
        }

        return CreateHudFont();
    }

    void DestroyOpenGlContext()
    {
        DestroyHudFont();

        if (gGlContext)
        {
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(gGlContext);
            gGlContext = NULL;
        }

        if (gWindowDC && gWindow)
        {
            ReleaseDC(gWindow, gWindowDC);
            gWindowDC = NULL;
        }
    }

    HMENU CreateMainMenuBar()
    {
        HMENU menuBar = CreateMenu();
        HMENU fileMenu = CreatePopupMenu();
        HMENU editMenu = CreatePopupMenu();
        HMENU drawMenu = CreatePopupMenu();

        AppendMenuA(fileMenu, MF_STRING, ID_FILE_NEW, "New");
        AppendMenuA(fileMenu, MF_STRING, ID_FILE_OPEN, "Open...\tCtrl+O");
        AppendMenuA(fileMenu, MF_STRING, ID_FILE_SAVE, "Save\tCtrl+S");
        AppendMenuA(fileMenu, MF_STRING, ID_FILE_SAVE_AS, "Save As...");
        AppendMenuA(fileMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(fileMenu, MF_STRING, ID_EXPORT_SVG, "Export SVG...");
        AppendMenuA(fileMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(fileMenu, MF_STRING, ID_FILE_EXIT, "Exit");

        AppendMenuA(editMenu, MF_STRING, ID_UNDO, "Undo\tCtrl+Z");
        AppendMenuA(editMenu, MF_STRING, ID_REDO, "Redo\tCtrl+Y");
        AppendMenuA(editMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(editMenu, MF_STRING, ID_DELETE_SEGMENT, "Delete\tDel");

        AppendMenuA(drawMenu, MF_STRING, ID_MODE_LINE, "Line");
        AppendMenuA(drawMenu, MF_STRING, ID_MODE_ARC, "Arc");
        AppendMenuA(drawMenu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(drawMenu, MF_STRING, ID_FINISH, "Finish\tEsc");
        AppendMenuA(drawMenu, MF_STRING, ID_CONTINUE, "Continue\tEnter");

        AppendMenuA(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), "File");
        AppendMenuA(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(editMenu), "Edit");
        AppendMenuA(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(drawMenu), "Draw");
        return menuBar;
    }

    void UpdateMainMenuState()
    {
        if (!gWindow)
        {
            return;
        }

        HMENU menuBar = GetMenu(gWindow);
        if (!menuBar)
        {
            return;
        }

        CheckMenuItem(menuBar, ID_MODE_LINE, MF_BYCOMMAND | (gInsertMode == InsertLine ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menuBar, ID_MODE_ARC, MF_BYCOMMAND | (gInsertMode == InsertArc ? MF_CHECKED : MF_UNCHECKED));

        EnableMenuItem(menuBar, ID_FINISH, MF_BYCOMMAND | (gActivePolylineIndex >= 0 ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menuBar, ID_CONTINUE, MF_BYCOMMAND | (CanContinueLastPolyline() ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menuBar, ID_UNDO, MF_BYCOMMAND | (CanUndo() ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menuBar, ID_REDO, MF_BYCOMMAND | (CanRedo() ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menuBar, ID_DELETE_SEGMENT, MF_BYCOMMAND | (HasSelectedSegment() && gActivePolylineIndex < 0 ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menuBar, ID_EXPORT_SVG, MF_BYCOMMAND | (CanExportSvg() ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menuBar, ID_FILE_SAVE, MF_BYCOMMAND | (CanExportSvg() ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(menuBar, ID_FILE_SAVE_AS, MF_BYCOMMAND | (CanExportSvg() ? MF_ENABLED : MF_GRAYED));

        DrawMenuBar(gWindow);
    }

    void ShowContextMenu(HWND hwnd, POINT screenPoint)
    {
        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }

        AppendMenuA(menu, MF_STRING | (gInsertMode == InsertLine ? MF_CHECKED : 0), ID_MODE_LINE, "Line");
        AppendMenuA(menu, MF_STRING | (gInsertMode == InsertArc ? MF_CHECKED : 0), ID_MODE_ARC, "Arc");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, ID_FILE_OPEN, "Open SVG...");
        AppendMenuA(menu, (CanExportSvg() ? MF_STRING : MF_GRAYED), ID_FILE_SAVE, "Save");
        AppendMenuA(menu, (CanExportSvg() ? MF_STRING : MF_GRAYED), ID_FILE_SAVE_AS, "Save As...");
        AppendMenuA(menu, (CanExportSvg() ? MF_STRING : MF_GRAYED), ID_EXPORT_SVG, "Export SVG...");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, (gActivePolylineIndex >= 0 ? MF_STRING : MF_GRAYED), ID_FINISH, "Finish\tEsc");
        AppendMenuA(menu, (CanContinueLastPolyline() ? MF_STRING : MF_GRAYED), ID_CONTINUE, "Continue\tEnter");
        AppendMenuA(menu, (CanUndo() ? MF_STRING : MF_GRAYED), ID_UNDO, "Undo\tCtrl+Z");
        AppendMenuA(menu, (CanRedo() ? MF_STRING : MF_GRAYED), ID_REDO, "Redo\tCtrl+Y");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, (HasSelectedSegment() && gActivePolylineIndex < 0 ? MF_STRING : MF_GRAYED), ID_DELETE_SEGMENT, "Delete\tDel");

        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd, NULL);
        DestroyMenu(menu);

        if (command != 0)
        {
            SendMessage(hwnd, WM_COMMAND, MAKEWPARAM(command, 0), 0);
        }
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
            case WM_CREATE:
            {
                gWindow = hwnd;
                SetMenu(hwnd, CreateMainMenuBar());
                if (!CreateOpenGlContext(hwnd))
                {
                    MessageBoxA(hwnd, "OpenGL context creation failed.", "Error", MB_OK | MB_ICONERROR);
                    return -1;
                }

                UpdateWindowTitle();
                UpdateMainMenuState();
                return 0;
            }

            case WM_SIZE:
            {
                gClientWidth = LOWORD(lParam);
                gClientHeight = HIWORD(lParam);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            case WM_ERASEBKGND:
            {
                return 1;
            }

            case WM_MOUSEMOVE:
            {
                gMouseScreenPosition.x = GET_X_LPARAM(lParam);
                gMouseScreenPosition.y = GET_Y_LPARAM(lParam);

                if (gIsPanning)
                {
                    const int deltaX = gMouseScreenPosition.x - gLastPanScreenPosition.x;
                    const int deltaY = gMouseScreenPosition.y - gLastPanScreenPosition.y;
                    gViewOrigin.x -= static_cast<double>(deltaX) / gZoom;
                    gViewOrigin.y -= static_cast<double>(deltaY) / gZoom;
                    gLastPanScreenPosition = gMouseScreenPosition;
                }

                gMousePosition = ScreenToWorld(gMouseScreenPosition);
                UpdateSnapHighlight();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            case WM_LBUTTONDOWN:
            {
                gMouseScreenPosition.x = GET_X_LPARAM(lParam);
                gMouseScreenPosition.y = GET_Y_LPARAM(lParam);
                const Vec2 point = ScreenToWorld(gMouseScreenPosition);
                gMousePosition = point;

                const bool nearSnapVertex = IsPointNearSnapVertex(point);
                if (gActivePolylineIndex < 0 && !nearSnapVertex && TrySelectSegmentAtPoint(point))
                {
                    InvalidateEditor();
                    return 0;
                }

                if (gActivePolylineIndex < 0)
                {
                    ClearSelectedSegment();
                }

                if (gInsertMode == InsertLine)
                {
                    AddLinePoint(point);
                }
                else
                {
                    HandleArcClick(point);
                }
                return 0;
            }

            case WM_MBUTTONDOWN:
            {
                gIsPanning = true;
                gLastPanScreenPosition.x = GET_X_LPARAM(lParam);
                gLastPanScreenPosition.y = GET_Y_LPARAM(lParam);
                SetCapture(hwnd);
                return 0;
            }

            case WM_MBUTTONUP:
            {
                gIsPanning = false;
                ReleaseCapture();
                return 0;
            }

            case WM_MOUSEWHEEL:
            {
                POINT screenPoint;
                screenPoint.x = GET_X_LPARAM(lParam);
                screenPoint.y = GET_Y_LPARAM(lParam);
                ScreenToClient(hwnd, &screenPoint);

                const Vec2 worldBeforeZoom = ScreenToWorld(screenPoint);
                const short wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                const double zoomFactor = std::pow(1.1, static_cast<double>(wheelDelta) / WHEEL_DELTA);
                gZoom = std::max(kMinZoom, std::min(kMaxZoom, gZoom * zoomFactor));

                const Vec2 worldAfterZoom = ScreenToWorld(screenPoint);
                gViewOrigin = gViewOrigin + (worldBeforeZoom - worldAfterZoom);

                gMouseScreenPosition = screenPoint;
                gMousePosition = ScreenToWorld(gMouseScreenPosition);
                UpdateSnapHighlight();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            case WM_CONTEXTMENU:
            {
                POINT screenPoint;
                screenPoint.x = GET_X_LPARAM(lParam);
                screenPoint.y = GET_Y_LPARAM(lParam);
                if (screenPoint.x == -1 && screenPoint.y == -1)
                {
                    GetCursorPos(&screenPoint);
                }

                if (gActivePolylineIndex < 0)
                {
                    POINT clientPoint = screenPoint;
                    ScreenToClient(hwnd, &clientPoint);
                    gMouseScreenPosition = clientPoint;
                    gMousePosition = ScreenToWorld(clientPoint);
                    if (!IsPointNearSnapVertex(gMousePosition))
                    {
                        TrySelectSegmentAtPoint(gMousePosition);
                    }
                }

                ShowContextMenu(hwnd, screenPoint);
                return 0;
            }

            case WM_KEYDOWN:
            {
                const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

                if (wParam == VK_ESCAPE && !gDistanceInputBuffer.empty())
                {
                    ClearDistanceInput();
                    UpdateSnapHighlight();
                    InvalidateEditor();
                    return 0;
                }

                if (wParam == VK_ESCAPE && HasSelectedSegment())
                {
                    ClearSelectedSegment();
                    InvalidateEditor();
                    return 0;
                }

                if (wParam == VK_ESCAPE)
                {
                    FinishCurrentPolyline();
                    return 0;
                }

                if (wParam == VK_DELETE)
                {
                    DeleteSelectedSegment();
                    return 0;
                }

                if (wParam == VK_RETURN)
                {
                    if (CanUseTypedDistance())
                    {
                        const Polyline& polyline = gPolylines[gActivePolylineIndex];
                        Vec2 typedEndPoint;
                        if (TryGetTypedEndpoint(polyline, typedEndPoint))
                        {
                            if (gInsertMode == InsertLine)
                            {
                                AddLinePoint(typedEndPoint);
                            }
                            else
                            {
                                SetPendingArcEnd(typedEndPoint);
                            }
                            return 0;
                        }

                        if (!gDistanceInputBuffer.empty())
                        {
                            return 0;
                        }
                    }

                    if (CanUseTypedArcRadius())
                    {
                        const Polyline& polyline = gPolylines[gActivePolylineIndex];
                        double radius = 0.0;
                        if (TryGetTypedDistance(radius))
                        {
                            Segment segment;
                            if (BuildArcSegmentFromRadius(polyline, gPendingArcEnd, radius, gMousePosition, segment))
                            {
                                AddArcSegment(segment);
                                return 0;
                            }
                        }

                        if (!gDistanceInputBuffer.empty())
                        {
                            return 0;
                        }
                    }

                    ContinueLastPolyline();
                    return 0;
                }

                if (ctrlDown && !shiftDown && (wParam == 'Z' || wParam == 'z'))
                {
                    UndoLastStep();
                    return 0;
                }

                if (ctrlDown && !shiftDown && (wParam == 'O' || wParam == 'o'))
                {
                    OpenDocument();
                    return 0;
                }

                if (ctrlDown && !shiftDown && (wParam == 'S' || wParam == 's'))
                {
                    SaveDocument();
                    return 0;
                }

                if (ctrlDown && !shiftDown && (wParam == 'Y' || wParam == 'y'))
                {
                    RedoLastStep();
                    return 0;
                }

                return 0;
            }

            case WM_CHAR:
            {
                if (CanUseTypedDistance() || CanUseTypedArcRadius())
                {
                    if ((wParam >= '0' && wParam <= '9') || wParam == '.' || wParam == ',')
                    {
                        if (CanUseTypedDistance())
                        {
                            const Polyline& polyline = gPolylines[gActivePolylineIndex];
                            const Vec2 anchor = GetCurrentEndPoint(polyline);

                            if (!gHasDistanceDirection)
                            {
                                LockDistanceDirection(anchor, gMousePosition);
                                if (!gHasDistanceDirection)
                                {
                                    return 0;
                                }
                            }
                        }

                        gDistanceInputBuffer.push_back(static_cast<char>(wParam));
                        InvalidateEditor();
                        return 0;
                    }

                    if (wParam == '\b' && !gDistanceInputBuffer.empty())
                    {
                        gDistanceInputBuffer.erase(gDistanceInputBuffer.end() - 1);
                        if (gDistanceInputBuffer.empty())
                        {
                            gHasDistanceDirection = false;
                            gDistanceDirection = Vec2();
                        }
                        InvalidateEditor();
                        return 0;
                    }
                }

                return 0;
            }

            case WM_COMMAND:
            {
                switch (LOWORD(wParam))
                {
                    case ID_MODE_LINE:
                        gInsertMode = InsertLine;
                        gHasPendingArcEnd = false;
                        ClearDistanceInput();
                        UpdateSnapHighlight();
                        InvalidateEditor();
                        return 0;

                    case ID_MODE_ARC:
                        gInsertMode = InsertArc;
                        gHasPendingArcEnd = false;
                        ClearDistanceInput();
                        UpdateSnapHighlight();
                        InvalidateEditor();
                        return 0;

                    case ID_FILE_NEW:
                        NewDocument();
                        return 0;

                    case ID_FILE_OPEN:
                        OpenDocument();
                        return 0;

                    case ID_FILE_SAVE:
                        SaveDocument();
                        return 0;

                    case ID_FILE_SAVE_AS:
                        SaveDocumentAs();
                        return 0;

                    case ID_FILE_EXIT:
                        DestroyWindow(hwnd);
                        return 0;

                    case ID_FINISH:
                        FinishCurrentPolyline();
                        return 0;

                    case ID_CONTINUE:
                        ContinueLastPolyline();
                        return 0;

                    case ID_UNDO:
                        UndoLastStep();
                        return 0;

                    case ID_REDO:
                        RedoLastStep();
                        return 0;

                    case ID_EXPORT_SVG:
                        ExportSvg();
                        return 0;

                    case ID_DELETE_SEGMENT:
                        DeleteSelectedSegment();
                        return 0;
                }
                return 0;
            }

            case WM_PAINT:
            {
                PAINTSTRUCT paintStruct;
                HDC hdc = BeginPaint(hwnd, &paintStruct);
                RenderScene(hdc);
                EndPaint(hwnd, &paintStruct);
                return 0;
            }

            case WM_DESTROY:
            {
                DestroyOpenGlContext();
                PostQuitMessage(0);
                return 0;
            }
        }

        return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand)
{
    WNDCLASSA windowClass;
    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = "CadProjektPolylineEditor";

    if (!RegisterClassA(&windowClass))
    {
        MessageBoxA(NULL, "Window class registration failed.", "Error", MB_OK | MB_ICONERROR);
        return 0;
    }

    HWND hwnd = CreateWindowA(
        "CadProjektPolylineEditor",
        "Zadanie testowe 2",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        kDefaultWidth,
        kDefaultHeight,
        NULL,
        NULL,
        instance,
        NULL
    );

    if (!hwnd)
    {
        MessageBoxA(NULL, "Window creation failed.", "Error", MB_OK | MB_ICONERROR);
        return 0;
    }

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG message;
    while (GetMessage(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    return static_cast<int>(message.wParam);
}
