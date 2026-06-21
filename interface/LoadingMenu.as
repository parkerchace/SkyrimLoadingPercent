/**
 * SkyrimLoadingPercent — LoadingMenu.as
 * ActionScript 2.0 (Flash 8 / GFx 4)
 * Compile with MTASC:
 *   mtasc -swf LoadingMenu.swf -main -header 1280:720:30 -version 8 -mx LoadingMenu.as
 *
 * The C++ SKSE plugin registers these global functions on the root:
 *   GetLoadingProgress() -> Number  (0–100)
 *   GetAnimStyle()       -> Number  (0–9)
 *   GetShowPercent()     -> Boolean
 *   GetScale()           -> Number
 *   GetColor()           -> Number  (RGB hex)
 */

class LoadingMenu {

    // ── State ────────────────────────────────────────────────────────────────
    static var _mc:MovieClip;       // Root MovieClip reference
    static var _canvas:MovieClip;   // Drawing surface
    static var _pctTF:TextField;    // Percentage label
    static var _tipTF:TextField;    // Loading tip (vanilla compat)
    static var _tick:Number   = 0;
    static var _progress:Number = 0;
    static var _target:Number   = 0;
    static var _style:Number    = 0;
    static var _showPct:Boolean = true;
    static var _scale:Number    = 1.0;
    static var _color:Number    = 0xC8902A;

    // Per-animation persistent data
    static var _blockOrder:Array;   // style 4: pre-shuffled pixel order
    static var _blockLit:Array;
    static var _snowDone:Boolean = false;

    // ── Entry point ──────────────────────────────────────────────────────────
    static function main(root:MovieClip):Void {
        _mc = root;

        // Vanilla-compatibility stubs (game may try to write these)
        var lm:MovieClip = _mc.createEmptyMovieClip("LoadingMenu_mc", 1);
        lm.createEmptyMovieClip("LoadBar", 1);
        var lt:TextField = lm.createTextField("LoadingText", 2, 0, 0, 1280, 40);
        lt.selectable = false;

        // Our main drawing canvas — centered on stage
        _canvas = _mc.createEmptyMovieClip("progress_canvas", 50);
        _canvas._x = 640;
        _canvas._y = 360;

        // Percentage text
        _pctTF = _mc.createTextField("pctTF", 51, 540, 430, 200, 36);
        _pctTF.selectable = false;
        _pctTF.autoSize = "center";
        var pFmt:TextFormat = new TextFormat();
        pFmt.font  = "_typewriter";
        pFmt.size  = 22;
        pFmt.color = 0xFFFFFF;
        pFmt.bold  = true;
        pFmt.align = "center";
        _pctTF.setNewTextFormat(pFmt);

        // Loading tip text at bottom (relay from LoadingMenu_mc.LoadingText)
        _tipTF = _mc.createTextField("tipTF", 52, 100, 655, 1080, 50);
        _tipTF.selectable = false;
        _tipTF.autoSize = "center";
        _tipTF.wordWrap = true;
        var tFmt:TextFormat = new TextFormat();
        tFmt.font  = "_sans";
        tFmt.size  = 14;
        tFmt.color = 0xCCCCCC;
        tFmt.align = "center";
        _tipTF.setNewTextFormat(tFmt);

        // Read C++ config (fallback to defaults if functions not injected yet)
        _style   = (typeof _mc["GetAnimStyle"]   == "function") ? _mc["GetAnimStyle"]()   : 0;
        _showPct = (typeof _mc["GetShowPercent"] == "function") ? _mc["GetShowPercent"]() : true;
        _scale   = (typeof _mc["GetScale"]       == "function") ? _mc["GetScale"]()       : 1.0;
        _color   = (typeof _mc["GetColor"]       == "function") ? _mc["GetColor"]()       : 0xC8902A;
        _canvas._xscale = _canvas._yscale = _scale * 100;

        // Pre-build pixel block shuffle for style 4
        _blockOrder = [];
        _blockLit   = [];
        for (var b:Number = 0; b < 64; b++) { _blockOrder.push(b); _blockLit.push(false); }
        fisherYates(_blockOrder);

        _mc.onEnterFrame = LoadingMenu.tick;
    }

    static function tick():Void {
        _tick++;

        // Poll real progress from C++
        _target = (typeof _mc["GetLoadingProgress"] == "function")
            ? _mc["GetLoadingProgress"]()
            : Math.min(99, _tick * 0.4);

        // Smooth lerp (factor ~0.06 at 30fps gives ~2-second smoothing window)
        _progress += (_target - _progress) * 0.06;

        // Draw chosen animation
        _canvas.clear();
        switch (_style) {
            case 0: drawCircleFill();    break;
            case 1: drawDragonSpin();    break;
            case 2: drawNordicRunes();   break;
            case 3: drawWaveformPulse(); break;
            case 4: drawPixelBlocks();   break;
            case 5: drawOrbitDots();     break;
            case 6: drawCompassRose();   break;
            case 7: drawHelixSpiral();   break;
            case 8: drawSnowflake();     break;
            case 9: drawLinearBar();     break;
        }

        // Percentage label
        if (_showPct && _style != 9) {
            _pctTF._visible = true;
            _pctTF.text     = Math.floor(_progress) + "%";
        } else {
            _pctTF._visible = (_style == 9) ? false : _showPct;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 0 — CIRCLE FILL
    // Three concentric rings; middle arc fills clockwise with progress.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawCircleFill():Void {
        var cv:MovieClip = _canvas;
        var t:Number     = _tick;
        var p:Number     = _progress / 100;
        var glow:Number  = 0.5 + 0.5 * Math.sin(t * 0.08);

        // Outer decorative ring — tick marks every 30°
        cv.lineStyle(1, _color, 40);
        drawCircleStroke(cv, 0, 0, 92);
        cv.lineStyle(0, 0, 0);
        for (var i:Number = 0; i < 12; i++) {
            var a:Number = (i / 12) * 360 * DEG;
            cv.moveTo(Math.cos(a) * 86, Math.sin(a) * 86);
            cv.lineTo(Math.cos(a) * 92, Math.sin(a) * 92);
        }

        // Filled progress arc (r=75), from -90° clockwise
        if (p > 0) {
            cv.lineStyle(0, 0, 0);
            cv.beginFill(_color, 18);
            drawFilledArc(cv, 0, 0, 75, -90, -90 + 360 * p);
            cv.endFill();
            cv.lineStyle(4, _color, 80 + glow * 20);
            drawArc(cv, 0, 0, 75, -90, -90 + 360 * p);
        }

        // Background arc remainder
        cv.lineStyle(2, 0x333355, 60);
        if (p < 1) drawArc(cv, 0, 0, 75, -90 + 360 * p, 270);

        // Inner circle — pulses
        var ir:Number = 18 + 3 * glow;
        cv.lineStyle(0, 0, 0);
        cv.beginFill(_color, 30 + 20 * glow);
        drawFilledArc(cv, 0, 0, ir, 0, 360);
        cv.endFill();

        // Rotating sweep indicator at tip of arc
        var tipAngle:Number = (-90 + 360 * p) * DEG;
        cv.lineStyle(3, 0xFFFFFF, 90);
        cv.moveTo(Math.cos(tipAngle) * 65, Math.sin(tipAngle) * 65);
        cv.lineTo(Math.cos(tipAngle) * 85, Math.sin(tipAngle) * 85);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 1 — DRAGON SPIN
    // Stylised dragon-eye shape with pupil that expands as progress grows.
    // The whole form rotates; when fully loaded it flares open.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawDragonSpin():Void {
        var cv:MovieClip = _canvas;
        var p:Number     = _progress / 100;
        var rot:Number   = _tick * 1.2 * DEG;   // steady rotation

        // Eye shape (two arcs meeting at poles)
        var h:Number = 50 + 40 * p;  // eye opens as progress grows
        cv.lineStyle(3, _color, 90);
        // Top arc
        cv.moveTo(0, -h);
        var ww:Number = 70;
        for (var i:Number = 0; i <= 30; i++) {
            var frac:Number = i / 30;
            var ax:Number   = Math.sin(frac * Math.PI) * ww;
            var ay:Number   = -h + h * (1 - Math.cos(frac * Math.PI));
            if (i == 0) cv.moveTo(ax, ay); else cv.lineTo(ax, ay);
        }
        // Mirror bottom arc
        cv.moveTo(0, -h);
        for (var j:Number = 0; j <= 30; j++) {
            var fb:Number = j / 30;
            var bx:Number = Math.sin(fb * Math.PI) * ww;
            var by:Number = -h - h * (1 - Math.cos(fb * Math.PI)) + 2 * h;
            if (j == 0) cv.moveTo(bx, by); else cv.lineTo(bx, by);
        }

        // Pupil — ellipse that grows with p
        var pr:Number = 8 + 40 * p;
        cv.lineStyle(0, 0, 0);
        cv.beginFill(_color, 70);
        drawFilledArc(cv, 0, 0, pr * 0.6, 0, 360);
        cv.endFill();

        // Slit highlight
        cv.lineStyle(2, 0xFFFFDD, 80);
        cv.moveTo(0, -(pr * 0.5));
        cv.lineTo(0,  (pr * 0.5));

        // Wing lines radiating at 60° from centre — count grows with progress
        var wingCount:Number = Math.floor(p * 8);
        cv.lineStyle(1, _color, 35);
        for (var w:Number = 0; w < wingCount; w++) {
            var wa:Number = (w / 8) * 360 * DEG + rot;
            cv.moveTo(0, 0);
            cv.lineTo(Math.cos(wa) * 110, Math.sin(wa) * 110);
        }

        // Outer glow ring that scales in
        cv.lineStyle(2, _color, 20 + 30 * p);
        drawCircleStroke(cv, 0, 0, 100 * p);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 2 — NORDIC RUNES
    // Three concentric rings of dots, each rotating at a different speed.
    // Dots brighten as the progress frontier passes them.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawNordicRunes():Void {
        var cv:MovieClip = _canvas;
        var p:Number     = _progress / 100;
        var t:Number     = _tick;

        var rings:Array = [
            { n: 8,  r: 40,  speed: 0.6,  size: 5  },
            { n: 12, r: 65,  speed:-0.35, size: 4  },
            { n: 16, r: 88,  speed: 0.2,  size: 3  }
        ];

        var totalDots:Number = 36;
        var litCount:Number  = Math.floor(p * totalDots);
        var dotIdx:Number    = 0;

        for (var ri:Number = 0; ri < rings.length; ri++) {
            var ring   = rings[ri];
            var offset:Number = t * ring.speed * DEG;
            for (var di:Number = 0; di < ring.n; di++) {
                var angle:Number = (di / ring.n) * 360 * DEG + offset;
                var dx:Number    = Math.cos(angle) * ring.r;
                var dy:Number    = Math.sin(angle) * ring.r;
                var lit:Boolean  = (dotIdx < litCount);
                var alpha:Number = lit ? 90 : 20;
                var c:Number     = lit ? _color : 0x4466AA;
                cv.lineStyle(0, 0, 0);
                cv.beginFill(c, alpha);
                drawFilledArc(cv, dx, dy, ring.size, 0, 360);
                cv.endFill();
                dotIdx++;
            }
        }

        // Cardinal rune glyphs (I-shapes drawn with lines)
        cv.lineStyle(1, _color, 30);
        var cardinals:Array = [0, 90, 180, 270];
        for (var ci:Number = 0; ci < cardinals.length; ci++) {
            var ca:Number = cardinals[ci] * DEG;
            var cx2:Number = Math.cos(ca) * 100;
            var cy2:Number = Math.sin(ca) * 100;
            var px2:Number = Math.cos(ca + Math.PI / 2);
            var py2:Number = Math.sin(ca + Math.PI / 2);
            cv.moveTo(cx2 - px2 * 6, cy2 - py2 * 6);
            cv.lineTo(cx2 + px2 * 6, cy2 + py2 * 6);
            cv.moveTo(cx2, cy2 - 10);
            cv.lineTo(cx2, cy2 + 10);
        }

        // Central circle
        cv.lineStyle(2, _color, 50);
        drawCircleStroke(cv, 0, 0, 14);
        cv.lineStyle(0, 0, 0);
        cv.beginFill(_color, 40);
        drawFilledArc(cv, 0, 0, 10, 0, 360);
        cv.endFill();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 3 — WAVEFORM PULSE
    // 24 vertical bars arranged symmetrically; heights ripple with sine waves.
    // Overall amplitude is anchored to progress.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawWaveformPulse():Void {
        var cv:MovieClip = _canvas;
        var p:Number     = _progress / 100;
        var t:Number     = _tick;
        var barCount:Number = 24;
        var barW:Number     = 8;
        var spacing:Number  = 13;
        var totalW:Number   = barCount * spacing;
        var baseX:Number    = -totalW / 2 + spacing / 2;

        // Color: blue (0%) -> gold (100%)
        var r:Number = Math.floor(lerp(0x40, 0xC8, p));
        var g:Number = Math.floor(lerp(0x60, 0x90, p));
        var b:Number = Math.floor(lerp(0xCC, 0x2A, p));
        var barColor:Number = (r << 16) | (g << 8) | b;

        for (var i:Number = 0; i < barCount; i++) {
            var bx:Number = baseX + i * spacing;
            // Composite wave: two sine components
            var envelope:Number = 1 - Math.abs((i / (barCount - 1)) * 2 - 1); // peaks at centre
            var wave1:Number = Math.sin(i * 0.6 - t * 0.15) * 0.5 + 0.5;
            var wave2:Number = Math.sin(i * 1.1 + t * 0.08) * 0.25 + 0.25;
            var h:Number     = (wave1 + wave2) * envelope * 70 * p + 4;

            cv.lineStyle(0, 0, 0);
            cv.beginFill(barColor, 80);
            cv.moveTo(bx - barW / 2, -h);
            cv.lineTo(bx + barW / 2, -h);
            cv.lineTo(bx + barW / 2,  h);
            cv.lineTo(bx - barW / 2,  h);
            cv.lineTo(bx - barW / 2, -h);
            cv.endFill();

            // Top cap highlight
            cv.lineStyle(1, 0xFFFFFF, 40);
            cv.moveTo(bx - barW / 2, -h);
            cv.lineTo(bx + barW / 2, -h);
        }

        // Floor line
        cv.lineStyle(1, _color, 30);
        cv.moveTo(-totalW / 2, 0);
        cv.lineTo( totalW / 2, 0);

        // Percentage at right edge when showPercent
        if (_showPct) {
            _pctTF._visible = true;
            _pctTF.text     = Math.floor(_progress) + "%";
            _pctTF._x       = 640 + totalW / 2 + 10;
            _pctTF._y       = 348;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 4 — PIXEL BLOCKS
    // 8×8 grid of squares that light up in a pre-shuffled random order.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawPixelBlocks():Void {
        var cv:MovieClip = _canvas;
        var p:Number     = _progress / 100;
        var litTarget:Number = Math.floor(p * 64);

        // Light up new blocks this frame
        for (var k:Number = 0; k < 64; k++) {
            _blockLit[k] = (k < litTarget);
        }

        var blockSize:Number  = 18;
        var gap:Number        = 3;
        var step:Number       = blockSize + gap;
        var originX:Number    = -4 * step + gap / 2;
        var originY:Number    = -4 * step + gap / 2;

        for (var idx:Number = 0; idx < 64; idx++) {
            var cell:Number  = _blockOrder[idx];
            var col:Number   = cell % 8;
            var row:Number   = Math.floor(cell / 8);
            var bx:Number    = originX + col * step;
            var by:Number    = originY + row * step;
            var isLit:Boolean = _blockLit[idx];

            var fc:Number  = isLit ? _color : 0x222233;
            var fa:Number  = isLit ? 85 : 40;

            cv.lineStyle(1, isLit ? _color : 0x334455, isLit ? 60 : 25);
            cv.beginFill(fc, fa);
            cv.moveTo(bx,              by);
            cv.lineTo(bx + blockSize,  by);
            cv.lineTo(bx + blockSize,  by + blockSize);
            cv.lineTo(bx,              by + blockSize);
            cv.lineTo(bx,              by);
            cv.endFill();

            // Flash effect on newly lit block
            if (isLit && idx == litTarget - 1) {
                cv.lineStyle(0, 0, 0);
                cv.beginFill(0xFFFFFF, 60);
                cv.moveTo(bx + 2,             by + 2);
                cv.lineTo(bx + blockSize - 2, by + 2);
                cv.lineTo(bx + blockSize - 2, by + blockSize - 2);
                cv.lineTo(bx + 2,             by + blockSize - 2);
                cv.lineTo(bx + 2,             by + 2);
                cv.endFill();
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 5 — ORBIT DOTS
    // 12 dots on a ring. The "active cursor" dot orbits; each full lap activates
    // one more dot permanently, so completed dots = floor(progress/100 * 12).
    // ─────────────────────────────────────────────────────────────────────────
    static function drawOrbitDots():Void {
        var cv:MovieClip = _canvas;
        var p:Number     = _progress / 100;
        var t:Number     = _tick;
        var n:Number     = 12;
        var r:Number     = 70;
        var litN:Number  = Math.floor(p * n);
        var cursorAngle:Number = (t * 2.5) * DEG;

        for (var i:Number = 0; i < n; i++) {
            var dotAngle:Number = (i / n) * 360 * DEG - Math.PI / 2;
            var dx:Number = Math.cos(dotAngle) * r;
            var dy:Number = Math.sin(dotAngle) * r;
            var isActive:Boolean = (i < litN);

            // Trail for active dots
            if (isActive) {
                for (var tr:Number = 1; tr <= 5; tr++) {
                    var trA:Number = (i / n) * 360 * DEG - Math.PI / 2 - tr * 4 * DEG;
                    cv.lineStyle(0, 0, 0);
                    cv.beginFill(_color, 15 - tr * 2);
                    drawFilledArc(cv, Math.cos(trA) * r, Math.sin(trA) * r, 4 - tr * 0.5, 0, 360);
                    cv.endFill();
                }
            }

            // Main dot
            var dotR:Number = isActive ? 7 : 4;
            var dotA:Number = isActive ? 90 : 25;
            var dotC:Number = isActive ? _color : 0x334466;
            cv.lineStyle(0, 0, 0);
            cv.beginFill(dotC, dotA);
            drawFilledArc(cv, dx, dy, dotR, 0, 360);
            cv.endFill();
        }

        // Cursor dot orbiting ahead of the frontier
        cv.lineStyle(0, 0, 0);
        cv.beginFill(0xFFFFFF, 80);
        drawFilledArc(cv, Math.cos(cursorAngle) * r, Math.sin(cursorAngle) * r, 5, 0, 360);
        cv.endFill();

        // Outer guide ring
        cv.lineStyle(1, _color, 18);
        drawCircleStroke(cv, 0, 0, r);

        // Inner circle
        cv.lineStyle(2, _color, 40);
        drawCircleStroke(cv, 0, 0, 14);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 6 — COMPASS ROSE
    // 8-pointed star; cardinal points are tall, ordinal points are short.
    // Points reveal clockwise as progress increases.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawCompassRose():Void {
        var cv:MovieClip  = _canvas;
        var p:Number      = _progress / 100;
        var revealN:Number = Math.floor(p * 8);
        var glow:Number   = 0.5 + 0.5 * Math.sin(_tick * 0.07);

        for (var i:Number = 0; i < 8; i++) {
            var baseAngle:Number = (i / 8) * 360 * DEG - Math.PI / 2;
            var isCardinal:Boolean = (i % 2 == 0);
            var len:Number         = isCardinal ? 90 : 55;
            var wid:Number         = isCardinal ? 14 : 9;
            var revealed:Boolean   = (i < revealN);
            var fc2:Number         = revealed ? _color : 0x222244;
            var fa2:Number         = revealed ? 80     : 30;

            // Diamond point shape
            var tipX:Number  = Math.cos(baseAngle) * len;
            var tipY:Number  = Math.sin(baseAngle) * len;
            var sideA:Number = baseAngle + Math.PI / 2;
            var sideB:Number = baseAngle - Math.PI / 2;
            var baseX:Number = Math.cos(baseAngle) * 15;
            var baseY:Number = Math.sin(baseAngle) * 15;
            var midX:Number  = Math.cos(baseAngle) * (len * 0.45);
            var midY:Number  = Math.sin(baseAngle) * (len * 0.45);

            cv.lineStyle(1, fc2, fa2 + 10);
            cv.beginFill(fc2, fa2);
            cv.moveTo(baseX + Math.cos(sideA) * wid, baseY + Math.sin(sideA) * wid);
            cv.lineTo(midX + Math.cos(sideA) * (wid * 0.4), midY + Math.sin(sideA) * (wid * 0.4));
            cv.lineTo(tipX, tipY);
            cv.lineTo(midX + Math.cos(sideB) * (wid * 0.4), midY + Math.sin(sideB) * (wid * 0.4));
            cv.lineTo(baseX + Math.cos(sideB) * wid, baseY + Math.sin(sideB) * wid);
            cv.lineTo(baseX, baseY);
            cv.lineTo(baseX + Math.cos(sideA) * wid, baseY + Math.sin(sideA) * wid);
            cv.endFill();
        }

        // Central hub
        cv.lineStyle(2, _color, 60 + 20 * glow);
        drawCircleStroke(cv, 0, 0, 16);
        cv.lineStyle(0, 0, 0);
        cv.beginFill(_color, 50);
        drawFilledArc(cv, 0, 0, 12, 0, 360);
        cv.endFill();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 7 — HELIX SPIRAL
    // Two sine-wave helices drawn as a series of dots; height scales with p.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawHelixSpiral():Void {
        var cv:MovieClip  = _canvas;
        var p:Number      = _progress / 100;
        var t:Number      = _tick;
        var segs:Number   = 48;
        var height:Number = 90 * p;
        var twist:Number  = t * 0.05;

        for (var s:Number = 0; s <= segs; s++) {
            var frac:Number    = s / segs;
            var hy:Number      = -height + frac * height * 2;
            var angle1:Number  = frac * 4 * Math.PI + twist;
            var angle2:Number  = angle1 + Math.PI;
            var amplitude:Number = 35 + 10 * Math.sin(frac * Math.PI);

            var x1:Number = Math.cos(angle1) * amplitude;
            var x2:Number = Math.cos(angle2) * amplitude;

            var depth1:Number = Math.sin(angle1);   // –1 = back, +1 = front
            var depth2:Number = Math.sin(angle2);

            // Strand 1 (gold)
            var a1:Number = 30 + 55 * ((depth1 + 1) / 2);
            cv.lineStyle(0, 0, 0);
            cv.beginFill(_color, a1);
            drawFilledArc(cv, x1, hy, 3, 0, 360);
            cv.endFill();

            // Strand 2 (blue)
            var a2:Number = 30 + 55 * ((depth2 + 1) / 2);
            cv.beginFill(0x5599CC, a2);
            drawFilledArc(cv, x2, hy, 3, 0, 360);
            cv.endFill();

            // Connecting rung every 4 segments
            if (s % 4 == 0) {
                var lineAlpha:Number = 20 + 20 * Math.abs(depth1);
                cv.lineStyle(1, _color, lineAlpha);
                cv.moveTo(x1, hy);
                cv.lineTo(x2, hy);
            }
        }

        // Axis line
        cv.lineStyle(1, _color, 20);
        cv.moveTo(0, -height);
        cv.lineTo(0,  height);
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 8 — SNOWFLAKE
    // 6 arms grow outward from centre; each arm sprouts branches.
    // Arm count and length are tied to progress.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawSnowflake():Void {
        var cv:MovieClip = _canvas;
        var p:Number     = _progress / 100;
        var armMax:Number = 80;
        var armLen:Number = armMax * p;

        for (var arm:Number = 0; arm < 6; arm++) {
            var baseA:Number = (arm / 6) * 360 * DEG;

            cv.lineStyle(2, 0x88CCFF, 80);
            var ax:Number = Math.cos(baseA) * armLen;
            var ay:Number = Math.sin(baseA) * armLen;
            cv.moveTo(0, 0);
            cv.lineTo(ax, ay);

            // 3 branch pairs along each arm
            var branchCount:Number = 3;
            for (var br:Number = 1; br <= branchCount; br++) {
                var brFrac:Number = br / (branchCount + 1);
                var brLen:Number  = (1 - brFrac) * 28 * p;
                var brX:Number    = Math.cos(baseA) * (armLen * brFrac);
                var brY:Number    = Math.sin(baseA) * (armLen * brFrac);

                var brA1:Number = baseA + 60 * DEG;
                var brA2:Number = baseA - 60 * DEG;

                cv.lineStyle(1, 0xAADDFF, 70);
                cv.moveTo(brX, brY);
                cv.lineTo(brX + Math.cos(brA1) * brLen, brY + Math.sin(brA1) * brLen);
                cv.moveTo(brX, brY);
                cv.lineTo(brX + Math.cos(brA2) * brLen, brY + Math.sin(brA2) * brLen);
            }
        }

        // Sparkle at tip of each arm (only when arms are near full length)
        if (p > 0.85) {
            for (var sa:Number = 0; sa < 6; sa++) {
                var spa:Number = (sa / 6) * 360 * DEG;
                var sax:Number = Math.cos(spa) * armLen;
                var say:Number = Math.sin(spa) * armLen;
                var sparkAlpha:Number = 60 * Math.sin(_tick * 0.2 + sa);
                cv.lineStyle(0, 0, 0);
                cv.beginFill(0xFFFFFF, Math.abs(sparkAlpha));
                drawFilledArc(cv, sax, say, 4, 0, 360);
                cv.endFill();
            }
        }

        // Central gem
        cv.lineStyle(2, 0x88CCFF, 70);
        drawCircleStroke(cv, 0, 0, 8);
        cv.lineStyle(0, 0, 0);
        cv.beginFill(0xCCEEFF, 60);
        drawFilledArc(cv, 0, 0, 5, 0, 360);
        cv.endFill();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ANIMATION 9 — LINEAR BAR  (Elder Scrolls styled)
    // Classic horizontal progress bar with rune end-caps, scanlines, and
    // percentage. Text is handled directly inside here.
    // ─────────────────────────────────────────────────────────────────────────
    static function drawLinearBar():Void {
        var cv:MovieClip = _canvas;
        var p:Number     = _progress / 100;
        var barW:Number  = 700;
        var barH:Number  = 20;
        var bx:Number    = -barW / 2;
        var by:Number    = -barH / 2;

        // Track (background)
        cv.lineStyle(1, 0x444455, 80);
        cv.beginFill(0x0D0D1A, 90);
        roundRect(cv, bx - 2, by - 2, barW + 4, barH + 4, 3);
        cv.endFill();

        // Fill
        if (p > 0) {
            cv.lineStyle(0, 0, 0);
            cv.beginFill(_color, 85);
            roundRect(cv, bx, by, barW * p, barH, 2);
            cv.endFill();

            // Scanlines over the filled portion
            cv.lineStyle(0, 0, 0);
            cv.beginFill(0x000000, 18);
            for (var si:Number = 0; si < Math.floor(barW * p / 4); si++) {
                cv.moveTo(bx + si * 4,     by);
                cv.lineTo(bx + si * 4 + 2, by);
                cv.lineTo(bx + si * 4 + 2, by + barH);
                cv.lineTo(bx + si * 4,     by + barH);
                cv.lineTo(bx + si * 4,     by);
            }
            cv.endFill();

            // Shimmer pulse at the fill edge
            var pulseX:Number = bx + barW * p;
            cv.lineStyle(0, 0, 0);
            cv.beginFill(0xFFFFFF, 35 + 25 * Math.sin(_tick * 0.15));
            cv.moveTo(pulseX - 4, by);
            cv.lineTo(pulseX,     by);
            cv.lineTo(pulseX,     by + barH);
            cv.lineTo(pulseX - 4, by + barH);
            cv.lineTo(pulseX - 4, by);
            cv.endFill();
        }

        // Rune end-caps (octagonal shapes)
        var capSize:Number = 14;
        drawRuneCap(cv, bx - capSize / 2, 0, capSize);
        drawRuneCap(cv, bx + barW + capSize / 2, 0, capSize);

        // "LOADING" label above bar
        _pctTF._visible = false;
        if (_showPct && !_mc.loadBarTF) {
            _mc.createTextField("loadBarTF", 55, 540, 314, 200, 28);
            _mc.loadBarTF.selectable = false;
            _mc.loadBarTF.autoSize = "center";
            var lbFmt:TextFormat = new TextFormat();
            lbFmt.font  = "_typewriter";
            lbFmt.size  = 16;
            lbFmt.color = 0xFFFFFF;
            lbFmt.bold  = false;
            lbFmt.align = "center";
            lbFmt.letterSpacing = 4;
            _mc.loadBarTF.setNewTextFormat(lbFmt);
        }
        if (_mc.loadBarTF) {
            _mc.loadBarTF.text = "LOADING  " + Math.floor(_progress) + "%";
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // HELPER: rune cap decoration
    // ─────────────────────────────────────────────────────────────────────────
    static function drawRuneCap(cv:MovieClip, cx:Number, cy:Number, size:Number):Void {
        var s:Number = size;
        cv.lineStyle(2, _color, 70);
        cv.beginFill(_color, 25);
        cv.moveTo(cx,         cy - s);
        cv.lineTo(cx + s * 0.5, cy - s * 0.5);
        cv.lineTo(cx + s,     cy);
        cv.lineTo(cx + s * 0.5, cy + s * 0.5);
        cv.lineTo(cx,         cy + s);
        cv.lineTo(cx - s * 0.5, cy + s * 0.5);
        cv.lineTo(cx - s,     cy);
        cv.lineTo(cx - s * 0.5, cy - s * 0.5);
        cv.lineTo(cx,         cy - s);
        cv.endFill();
    }

    // ─────────────────────────────────────────────────────────────────────────
    // DRAWING UTILITIES
    // ─────────────────────────────────────────────────────────────────────────

    static var DEG:Number = Math.PI / 180;

    static function drawArc(mc:MovieClip, cx:Number, cy:Number, r:Number, startDeg:Number, endDeg:Number):Void {
        var steps:Number = Math.max(1, Math.ceil(Math.abs(endDeg - startDeg) / 2));
        var startRad:Number = startDeg * DEG;
        mc.moveTo(cx + r * Math.cos(startRad), cy + r * Math.sin(startRad));
        for (var i:Number = 1; i <= steps; i++) {
            var rad:Number = (startDeg + (endDeg - startDeg) * (i / steps)) * DEG;
            mc.lineTo(cx + r * Math.cos(rad), cy + r * Math.sin(rad));
        }
    }

    static function drawCircleStroke(mc:MovieClip, cx:Number, cy:Number, r:Number):Void {
        drawArc(mc, cx, cy, r, 0, 360);
    }

    static function drawFilledArc(mc:MovieClip, cx:Number, cy:Number, r:Number, startDeg:Number, endDeg:Number):Void {
        mc.moveTo(cx, cy);
        var steps:Number    = Math.max(1, Math.ceil(Math.abs(endDeg - startDeg) / 2));
        var startRad:Number = startDeg * DEG;
        mc.lineTo(cx + r * Math.cos(startRad), cy + r * Math.sin(startRad));
        for (var i:Number = 1; i <= steps; i++) {
            var rad:Number = (startDeg + (endDeg - startDeg) * (i / steps)) * DEG;
            mc.lineTo(cx + r * Math.cos(rad), cy + r * Math.sin(rad));
        }
        mc.lineTo(cx, cy);
    }

    static function roundRect(mc:MovieClip, x:Number, y:Number, w:Number, h:Number, r:Number):Void {
        mc.moveTo(x + r, y);
        mc.lineTo(x + w - r, y);
        mc.curveTo(x + w, y,     x + w, y + r);
        mc.lineTo(x + w, y + h - r);
        mc.curveTo(x + w, y + h, x + w - r, y + h);
        mc.lineTo(x + r, y + h);
        mc.curveTo(x,    y + h, x, y + h - r);
        mc.lineTo(x,     y + r);
        mc.curveTo(x,    y,     x + r, y);
    }

    static function lerp(a:Number, b:Number, t:Number):Number {
        return a + (b - a) * t;
    }

    static function fisherYates(arr:Array):Void {
        for (var n:Number = arr.length - 1; n > 0; n--) {
            var k:Number   = Math.floor(Math.random() * (n + 1));
            var tmp        = arr[n];
            arr[n]         = arr[k];
            arr[k]         = tmp;
        }
    }
}
