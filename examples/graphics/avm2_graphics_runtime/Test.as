package {
	import flash.display.Sprite;
	import flash.display.BitmapData;
	import flash.geom.Matrix;

	// T4 Part B getPixel gate for flash.display.Graphics RUNTIME drawing.
	// bd.draw(art) rasterizes the script-drawn geometry through the same CPU
	// path the headless getPixel gate reads (avm2_graphics_cpu_composite), so
	// CPU == GPU == Ruffle. All asserted points are exact by construction:
	//   - solid / stroke fills = the literal fill colour,
	//   - gradient endpoints sampled in the PAD region (t clamped to 0/1) = the
	//     pure end-stop colours (interpolation-independent),
	//   - drawTriangles interior = the current fill colour.
	// The gradient MID-point (interpolation-dependent) is traced informationally.
	public class Test extends Sprite {
		public function Test() {
			var art:Sprite = new Sprite();
			var g = art.graphics;

			// 1. Solid red rect, interior (40,40).
			g.beginFill(0xFF0000);
			g.drawRect(10, 10, 60, 60);
			g.endFill();

			// 2. drawTriangles cyan quad (two tris), interior (140,40).
			g.beginFill(0x00FFFF);
			g.drawTriangles(Vector.<Number>([
				100, 10, 160, 10, 160, 70,
				100, 10, 100, 70, 160, 70
			]));
			g.endFill();

			// 3. Linear gradient red->green, box centred at x=100 spanning
			//    x=80..120; the rect is wider so x<80 pads red, x>120 pads green.
			var m:Matrix = new Matrix();
			m.createGradientBox(40, 40, 0, 80, 120);
			g.beginGradientFill("linear",
				[0xFF0000, 0x00FF00], [1, 1], [0, 255], m);
			g.drawRect(10, 120, 180, 40);
			g.endFill();

			// 4. Blue stroke line, 8px wide, centred on y=100.
			g.lineStyle(8, 0x0000FF);
			g.moveTo(20, 100);
			g.lineTo(160, 100);

			addChild(art);

			var bd:BitmapData = new BitmapData(200, 200, false, 0xFFFFFF);
			bd.draw(art);
			emit(bd, "bg", 5, 5);              // ffffff
			emit(bd, "solid", 40, 40);         // ff0000
			emit(bd, "triangles", 140, 40);    // ffff
			emit(bd, "grad_start", 30, 140);   // ff0000 (pad red)
			emit(bd, "grad_end", 175, 140);    // ff00   (pad green)
			emit(bd, "stroke", 90, 100);       // ff
			// informational: gradient centre (interpolation-dependent).
			trace("grad_mid=" + bd.getPixel(100, 140).toString(16));
		}
		private function emit(bd:BitmapData, name:String, x:int, y:int):void {
			trace(name + "=" + bd.getPixel(x, y).toString(16));
		}
	}
}
