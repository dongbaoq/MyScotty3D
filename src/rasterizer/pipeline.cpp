// clang-format off
#include "pipeline.h"

#include <iostream>

#include "../lib/log.h"
#include "../lib/mathlib.h"
#include "framebuffer.h"
#include "sample_pattern.h"
template<PrimitiveType primitive_type, class Program, uint32_t flags>
void Pipeline<primitive_type, Program, flags>::run(std::vector<Vertex> const& vertices,
                                                   typename Program::Parameters const& parameters,
                                                   Framebuffer* framebuffer_) {
	// Framebuffer must be non-null:
	assert(framebuffer_);
	auto& framebuffer = *framebuffer_;

	// A1T7: sample loop
	// TODO: update this function to rasterize to *all* sample locations in the framebuffer.
	//  	 This will probably involve inserting a loop of the form:
	// 		 	std::vector< Vec3 > const &samples = framebuffer.sample_pattern.centers_and_weights;
	//      	for (uint32_t s = 0; s < samples.size(); ++s) { ... }
	//   	 around some subset of the code.
	// 		 You will also need to transform the input and output of the rasterize_* functions to
	// 	     account for the fact they deal with pixels centered at (0.5,0.5).

	std::vector<ShadedVertex> shaded_vertices;
	shaded_vertices.reserve(vertices.size());

	//--------------------------
	// shade vertices:
	for (auto const& v : vertices) {
		ShadedVertex sv;
		Program::shade_vertex(parameters, v.attributes, &sv.clip_position, &sv.attributes);
		shaded_vertices.emplace_back(sv);
	}

	//--------------------------
	// assemble + clip + homogeneous divide vertices:
	std::vector<ClippedVertex> clipped_vertices;

	// reserve some space to avoid reallocations later:
	if constexpr (primitive_type == PrimitiveType::Lines) {
		// clipping lines can never produce more than one vertex per input vertex:
		clipped_vertices.reserve(shaded_vertices.size());
	} else if constexpr (primitive_type == PrimitiveType::Triangles) {
		// clipping triangles can produce up to 8 vertices per input vertex:
		clipped_vertices.reserve(shaded_vertices.size() * 8);
	}
	// clang-format off

	//coefficients to map from clip coordinates to framebuffer (i.e., "viewport") coordinates:
	//x: [-1,1] -> [0,width]
	//y: [-1,1] -> [0,height]
	//z: [-1,1] -> [0,1] (OpenGL-style depth range)
	Vec3 const clip_to_fb_scale = Vec3{
		framebuffer.width / 2.0f,
		framebuffer.height / 2.0f,
		0.5f
	};
	Vec3 const clip_to_fb_offset = Vec3{
		0.5f * framebuffer.width,
		0.5f * framebuffer.height,
		0.5f
	};

	// helper used to put output of clipping functions into clipped_vertices:
	auto emit_vertex = [&](ShadedVertex const& sv) {
		ClippedVertex cv;
		float inv_w = 1.0f / sv.clip_position.w;
		cv.fb_position = clip_to_fb_scale * inv_w * sv.clip_position.xyz() + clip_to_fb_offset;
		cv.inv_w = inv_w;
		cv.attributes = sv.attributes;
		clipped_vertices.emplace_back(cv);
	};

	// actually do clipping:
	if constexpr (primitive_type == PrimitiveType::Lines) {
		for (uint32_t i = 0; i + 1 < shaded_vertices.size(); i += 2) {
			clip_line(shaded_vertices[i], shaded_vertices[i + 1], emit_vertex);
		}
	} else if constexpr (primitive_type == PrimitiveType::Triangles) {
		for (uint32_t i = 0; i + 2 < shaded_vertices.size(); i += 3) {
			clip_triangle(shaded_vertices[i], shaded_vertices[i + 1], shaded_vertices[i + 2], emit_vertex);
		}
	} else {
		static_assert(primitive_type == PrimitiveType::Lines, "Unsupported primitive type.");
	}

	//--------------------------
	// rasterize primitives:

	std::vector<Fragment> fragments;

	// helper used to put output of rasterization functions into fragments:
	auto emit_fragment = [&](Fragment const& f) { fragments.emplace_back(f); };

	// actually do rasterization:
	if constexpr (primitive_type == PrimitiveType::Lines) {
		for (uint32_t i = 0; i + 1 < clipped_vertices.size(); i += 2) {
			rasterize_line(clipped_vertices[i], clipped_vertices[i + 1], emit_fragment);
		}
	} else if constexpr (primitive_type == PrimitiveType::Triangles) {
		for (uint32_t i = 0; i + 2 < clipped_vertices.size(); i += 3) {
			rasterize_triangle(clipped_vertices[i], clipped_vertices[i + 1], clipped_vertices[i + 2], emit_fragment);
		}
	} else {
		static_assert(primitive_type == PrimitiveType::Lines, "Unsupported primitive type.");
	}

	//--------------------------
	// depth test + shade + blend fragments:
	uint32_t out_of_range = 0; // check if rasterization produced fragments outside framebuffer 
							   // (indicates something is wrong with clipping)
	for (auto const& f : fragments) {

		// fragment location (in pixels):
		int32_t x = (int32_t)std::floor(f.fb_position.x);
		int32_t y = (int32_t)std::floor(f.fb_position.y);

		// if clipping is working properly, this condition shouldn't be needed;
		// however, it prevents crashes while you are working on your clipping functions,
		// so we suggest leaving it in place:
		if (x < 0 || (uint32_t)x >= framebuffer.width || 
		    y < 0 || (uint32_t)y >= framebuffer.height) {
			++out_of_range;
			continue;
		}

		// local names that refer to destination sample in framebuffer:
		float& fb_depth = framebuffer.depth_at(x, y, 0);
		Spectrum& fb_color = framebuffer.color_at(x, y, 0);


		// depth test:
		if constexpr ((flags & PipelineMask_Depth) == Pipeline_Depth_Always) {
			// "Always" means the depth test always passes.
		} else if constexpr ((flags & PipelineMask_Depth) == Pipeline_Depth_Never) {
			// "Never" means the depth test never passes.
			continue; //discard this fragment
		} else if constexpr ((flags & PipelineMask_Depth) == Pipeline_Depth_Less) {
			// "Less" means the depth test passes when the new fragment has depth less than the stored depth.
			// A1T4: Depth_Less
			// TODO: implement depth test! We want to only emit fragments that have a depth less than the stored depth, hence "Depth_Less".
		} else {
			static_assert((flags & PipelineMask_Depth) <= Pipeline_Depth_Always, "Unknown depth test flag.");
		}

		// if depth test passes, and depth writes aren't disabled, write depth to depth buffer:
		if constexpr (!(flags & Pipeline_DepthWriteDisableBit)) {
			fb_depth = f.fb_position.z;
		}

		// shade fragment:
		ShadedFragment sf;
		sf.fb_position = f.fb_position;
		Program::shade_fragment(parameters, f.attributes, f.derivatives, &sf.color, &sf.opacity);

		// write color to framebuffer if color writes aren't disabled:
		if constexpr (!(flags & Pipeline_ColorWriteDisableBit)) {
			// blend fragment:
			if constexpr ((flags & PipelineMask_Blend) == Pipeline_Blend_Replace) {
				fb_color = sf.color;
			} else if constexpr ((flags & PipelineMask_Blend) == Pipeline_Blend_Add) {
				// A1T4: Blend_Add
				// TODO: framebuffer color should have fragment color multiplied by fragment opacity added to it.
				fb_color = sf.color; //<-- replace this line
			} else if constexpr ((flags & PipelineMask_Blend) == Pipeline_Blend_Over) {
				// A1T4: Blend_Over
				// TODO: set framebuffer color to the result of "over" blending (also called "alpha blending") the fragment color over the framebuffer color, using the fragment's opacity
				// 		 You may assume that the framebuffer color has its alpha premultiplied already, and you just want to compute the resulting composite color
				fb_color = sf.color; //<-- replace this line
			} else {
				static_assert((flags & PipelineMask_Blend) <= Pipeline_Blend_Over, "Unknown blending flag.");
			}
		}
	}
	if (out_of_range > 0) {
		if constexpr (primitive_type == PrimitiveType::Lines) {
			warn("Produced %d fragments outside framebuffer; this indicates something is likely "
			     "wrong with the clip_line function.",
			     out_of_range);
		} else if constexpr (primitive_type == PrimitiveType::Triangles) {
			warn("Produced %d fragments outside framebuffer; this indicates something is likely "
			     "wrong with the clip_triangle function.",
			     out_of_range);
		}
	}
}

// -------------------------------------------------------------------------
// clipping functions

// helper to interpolate between vertices:
template<PrimitiveType p, class P, uint32_t F>
auto Pipeline<p, P, F>::lerp(ShadedVertex const& a, ShadedVertex const& b, float t) -> ShadedVertex {
	ShadedVertex ret;
	ret.clip_position = (b.clip_position - a.clip_position) * t + a.clip_position;
	for (uint32_t i = 0; i < ret.attributes.size(); ++i) {
		ret.attributes[i] = (b.attributes[i] - a.attributes[i]) * t + a.attributes[i];
	}
	return ret;
}

/*
 * clip_line - clip line to portion with -w <= x,y,z <= w, emit vertices of clipped line (if non-empty)
 *  	va, vb: endpoints of line
 *  	emit_vertex: call to produce truncated line
 *
 * If clipping shortens the line, attributes of the shortened line should respect the pipeline's interpolation mode.
 * 
 * If no portion of the line remains after clipping, emit_vertex will not be called.
 *
 * The clipped line should have the same direction as the full line.
 */
template<PrimitiveType p, class P, uint32_t flags>
void Pipeline<p, P, flags>::clip_line(ShadedVertex const& va, ShadedVertex const& vb,
                                      std::function<void(ShadedVertex const&)> const& emit_vertex) {
	// Determine portion of line over which:
	// 		pt = (b-a) * t + a
	//  	-pt.w <= pt.x <= pt.w
	//  	-pt.w <= pt.y <= pt.w
	//  	-pt.w <= pt.z <= pt.w
	// ... as a range [min_t, max_t]:

	float min_t = 0.0f;
	float max_t = 1.0f;

	// want to set range of t for a bunch of equations like:
	//    a.x + t * ba.x <= a.w + t * ba.w
	// so here's a helper:
	auto clip_range = [&min_t, &max_t](float l, float dl, float r, float dr) {
		// restrict range such that:
		// l + t * dl <= r + t * dr
		// re-arranging:
		//  l - r <= t * (dr - dl)
		if (dr == dl) {
			// want: l - r <= 0
			if (l - r > 0.0f) {
				// works for none of range, so make range empty:
				min_t = 1.0f;
				max_t = 0.0f;
			}
		} else if (dr > dl) {
			// since dr - dl is positive:
			// want: (l - r) / (dr - dl) <= t
			min_t = std::max(min_t, (l - r) / (dr - dl));
		} else { // dr < dl
			// since dr - dl is negative:
			// want: (l - r) / (dr - dl) >= t
			max_t = std::min(max_t, (l - r) / (dr - dl));
		}
	};

	// local names for clip positions and their difference:
	Vec4 const& a = va.clip_position;
	Vec4 const& b = vb.clip_position;
	Vec4 const ba = b - a;

	// -a.w - t * ba.w <= a.x + t * ba.x <= a.w + t * ba.w
	clip_range(-a.w, -ba.w, a.x, ba.x);
	clip_range(a.x, ba.x, a.w, ba.w);
	// -a.w - t * ba.w <= a.y + t * ba.y <= a.w + t * ba.w
	clip_range(-a.w, -ba.w, a.y, ba.y);
	clip_range(a.y, ba.y, a.w, ba.w);
	// -a.w - t * ba.w <= a.z + t * ba.z <= a.w + t * ba.w
	clip_range(-a.w, -ba.w, a.z, ba.z);
	clip_range(a.z, ba.z, a.w, ba.w);

	if (min_t < max_t) {
		if (min_t == 0.0f) {
			emit_vertex(va);
		} else {
			ShadedVertex out = lerp(va, vb, min_t);
			// don't interpolate attributes if in flat shading mode:
			if constexpr ((flags & PipelineMask_Interp) == Pipeline_Interp_Flat) {
				out.attributes = va.attributes;
			}
			emit_vertex(out);
		}
		if (max_t == 1.0f) {
			emit_vertex(vb);
		} else {
			ShadedVertex out = lerp(va, vb, max_t);
			// don't interpolate attributes if in flat shading mode:
			if constexpr ((flags & PipelineMask_Interp) == Pipeline_Interp_Flat) {
				out.attributes = va.attributes;
			}
			emit_vertex(out);
		}
	}
}

/*
 * clip_triangle - clip triangle to portion with -w <= x,y,z <= w, emit resulting shape as triangles (if non-empty)
 *  	va, vb, vc: vertices of triangle
 *  	emit_vertex: call to produce clipped triangles (three calls per triangle)
 *
 * If clipping truncates the triangle, attributes of the new vertices should respect the pipeline's interpolation mode.
 * 
 * If no portion of the triangle remains after clipping, emit_vertex will not be called.
 *
 * The clipped triangle(s) should have the same winding order as the full triangle.
 */
template<PrimitiveType p, class P, uint32_t flags>
void Pipeline<p, P, flags>::clip_triangle(
	ShadedVertex const& va, ShadedVertex const& vb, ShadedVertex const& vc,
	std::function<void(ShadedVertex const&)> const& emit_vertex) {
	// A1EC: clip_triangle
	// TODO: correct code!
	emit_vertex(va);
	emit_vertex(vb);
	emit_vertex(vc);
}

// -------------------------------------------------------------------------
// rasterization functions

/*
 * rasterize_line:
 * calls emit_fragment( frag ) for every pixel "covered" by the line (va.fb_position.xy, vb.fb_position.xy).
 *
 *    a pixel (x,y) is "covered" by the line if it exits the inscribed diamond:
 * 
 *        (x+0.5,y+1)
 *        /        \
 *    (x,y+0.5)  (x+1,y+0.5)
 *        \        /
 *         (x+0.5,y)
 *
 *    to avoid ambiguity, we consider diamonds to contain their left and bottom points
 *    but not their top and right points. 
 * 
 * 	  since 45 degree lines breaks this rule, our rule in general is to rasterize the line as if its
 *    endpoints va and vb were at va + (e, e^2) and vb + (e, e^2) where no smaller nonzero e produces 
 *    a different rasterization result. 
 *    We will not explicitly check for 45 degree lines along the diamond edges (this will be extra credit),
 *    but you should be able to handle 45 degree lines in every other case (such as starting from pixel centers)
 *
 * for each such diamond, pass Fragment frag to emit_fragment, with:
 *  - frag.fb_position.xy set to the center (x+0.5,y+0.5)
 *  - frag.fb_position.z interpolated linearly between va.fb_position.z and vb.fb_position.z
 *  - frag.attributes set to va.attributes (line will only be used in Interp_Flat mode)
 *  - frag.derivatives set to all (0,0)
 *
 * when interpolating the depth (z) for the fragments, you may use any depth the line takes within the pixel
 * (i.e., you don't need to interpolate to, say, the closest point to the pixel center)
 *
 * If you wish to work in fixed point, check framebuffer.h for useful information about the framebuffer's dimensions.
 */
template<PrimitiveType p, class P, uint32_t flags>
void Pipeline<p, P, flags>::rasterize_line(
	ClippedVertex const& va, ClippedVertex const& vb,
	std::function<void(Fragment const&)> const& emit_fragment) {
	if constexpr ((flags & PipelineMask_Interp) != Pipeline_Interp_Flat) {
		assert(0 && "rasterize_line should only be invoked in flat interpolation mode.");
	}
	// A1T2: rasterize_line

	// TODO: Check out the block comment above this function for more information on how to fill in
	// this function!
	// The OpenGL specification section 3.5 may also come in handy.

	// Bresenham's Algorithm
	Fragment point;
	float ax = va.fb_position.x;
	float ay = va.fb_position.y;
	float bx = vb.fb_position.x;
	float by = vb.fb_position.y;
	bool reversed = false;
	float temp;

	// Inside one pixel then just exit
	if(std::floor(ax) == std::floor(bx) && std::floor(ay) == std::floor(by)){
		return;
	}

	// Vertical case
	if(std::abs(bx - ax) < FLT_EPSILON){
		std::cout << "Vertical case" << std::endl;
		Fragment point;
		if(ay > by) {
			temp = by;
			by = ay;
			ay = temp;
			reversed = true;
		}
		int x, y;
		x = std::floor(ax);
		y = std::floor(ay);
		if(std::abs(ay - y - 0.5) + std::abs(ax - x - 0.5) <= 0.5 || ay <= y + 0.5f){
			if(reversed){
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, va.fb_position.z);
			}else{
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, vb.fb_position.z);
			}
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}
		for(y = std::floor(ay) + 1; y <= std::floor(by) - 1; y++){
			if(reversed){
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, (va.fb_position.z * (y + 0.5 - ay) + vb.fb_position.z * (by - y - 0.5)) / (by - ay));
			}else{
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, (vb.fb_position.z * (y + 0.5 - ay) + va.fb_position.z * (by - y - 0.5)) / (by - ay));
			}
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}	
		if(std::abs(ay - y - 0.5) + std::abs(ax - x - 0.5) <= 0.5 || by >= y + 0.5f){
			if(reversed){
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, vb.fb_position.z);
			}else{
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, va.fb_position.z);
			}
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}
		return;
	}

	
	
	// Control ax < bx
	if(bx < ax){
		temp = ax;
		ax = bx;
		bx = temp;
		
		temp = ay;
		ay = by;
		by = temp;
		reversed = true;
	}


	float slope = (by - ay) / (bx - ax);
	
	// Horizontal case
	if(std::abs(slope) < FLT_EPSILON){
		std::cout << "Horizontal case" << std::endl;
		Fragment point;
		
		if(ax > bx) {
			temp = bx;
			bx = ax;
			ax = temp;
		}
		int x, y;
		x = std::floor(ax);
		y = std::floor(ay);
		if(std::abs(ay - y - 0.5) + std::abs(ax - x - 0.5) <= 0.5 || ax < x + 0.5f){
			if(reversed){
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, vb.fb_position.z);
			}else{
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, va.fb_position.z);
			}
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}
		for(x = std::floor(ax) + 1; x <= std::floor(bx) - 1; x++){
			if(reversed){
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, (va.fb_position.z * (x + 0.5 - ax) + vb.fb_position.z * (bx - x - 0.5)) / (bx - ax));
			}else{
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, (vb.fb_position.z * (x + 0.5 - ax) + va.fb_position.z * (bx - x - 0.5)) / (bx - ax));
			}
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}	
		if(std::abs(ay - y - 0.5) + std::abs(ax - x - 0.5) <= 0.5 || bx > x + 0.5f){
			if(reversed){
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, va.fb_position.z);
			}else{
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, vb.fb_position.z);
			}
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}
		return;
	}

	// Diagonal case
	if(std::abs(slope - 1) < FLT_EPSILON){
		std::cout << "Diagonal positive case" << std::endl;

		// Start from (floor(ax)+0.5, floor(ay)+0.5)
		int32_t x = std::floor(ax);
		int32_t y = std::floor(ay);
		float interpolate_z;
		if((ay - y) - (ax - x) < -0.5){
			x++;
		}else if((ay - y) - (ax - x) > 0.5){
			y++;
		}

		for( ; x <= std::floor(bx) - 1 ; x++){
			if(reversed){
				interpolate_z = (va.fb_position.z * (x + 0.5f - ax) + vb.fb_position.z * (bx - x - 0.5f))/(bx - ax);
			}else{
				interpolate_z = (vb.fb_position.z * (x + 0.5f - ax) + va.fb_position.z * (bx - x - 0.5f))/(bx - ax);
			}
			point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;

			y++;
		}

		if(std::abs(by - std::floor(by) - 0.5) + std::abs(bx - std::floor(bx) - 0.5) <= 0.5 || by - std::floor(by) + bx - std::floor(bx) > 1.5){
			if(reversed){
				interpolate_z = va.fb_position.z;
			}else{
				interpolate_z = vb.fb_position.z;
			}
			point.fb_position = Vec3(std::floor(bx) + 0.5f, std::floor(by) + 0.5f, interpolate_z);
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}

		return;
	}
	
	if(std::abs(slope + 1) < FLT_EPSILON){
		std::cout << "Diagonal negative case" << std::endl;
		// Start from (floor(ax)+0.5, floor(ay)+0.5)
		int32_t x = std::floor(ax);
		int32_t y = std::floor(ay);
		float interpolate_z;
		if((ay - y) + (ax - x) < 0.5){
			y--;
		}else if((ay - y) + (ax - x) > 1.5){
			x++;
		}

		for( ; x <= std::floor(bx) - 1 ; x++){
			if(reversed){
				interpolate_z = (va.fb_position.z * (x + 0.5f - ax) + vb.fb_position.z * (bx - x - 0.5f))/(bx - ax);
			}else{
				interpolate_z = (vb.fb_position.z * (x + 0.5f - ax) + va.fb_position.z * (bx - x - 0.5f))/(bx - ax);
			}
			point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;

			y--;
		}

		if(std::abs(by - std::floor(by) - 0.5) + std::abs(bx - std::floor(bx) - 0.5) <= 0.5 || (by - std::floor(by)) - (bx - std::floor(bx)) < -0.5){
			if(reversed){
				interpolate_z = va.fb_position.z;
			}else{
				interpolate_z = vb.fb_position.z;
			}
			point.fb_position = Vec3(std::floor(bx) + 0.5f, std::floor(by) + 0.5f, interpolate_z);
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}
	}

	// Control slope < 0
	int signslope = 1;
	if(slope < 0){
		signslope = -1;
		slope = (-1) * slope;
	}

	// General case, 0 < slope < 1
	if(slope < 1){
		// Start from (floor(ax)+0.5, floor(ay)+0.5)
		int32_t x = std::floor(ax);
		int32_t y = std::floor(ay);
		// Epsilon is line's intersection at x = (pixel x) + 0.5, measure from (pixel y)
		float epsilon = ay - y + (x + 0.5 - ax) * slope;
		float interpolate_z;
		
		std::cout << "Slope lower than 1 case : " << slope << std::endl;
		if(y + slope * (ax - x - 0.5) > ay) { x++; epsilon += slope; }
		else if(ay + (x + 0.5 - ax) * slope >= y + 1) { y+=signslope; epsilon -= 1; }
		else if(ax - x + ay - y > 1.5) { x++; y+=signslope; epsilon += (slope - 1); }

		Fragment point;
		for( ; x <= std::floor(bx) - 1 ; x++){
			if(reversed){
				interpolate_z = (va.fb_position.z * (x + 0.5f - ax) + vb.fb_position.z * (bx - x - 0.5f))/(bx - ax);
			}else{
				interpolate_z = (vb.fb_position.z * (x + 0.5f - ax) + va.fb_position.z * (bx - x - 0.5f))/(bx - ax);
			}
			
			point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;

			// Next pixel becomes (x+1, y+1)
			if(epsilon > 1 - slope) {
				y+=signslope;
				epsilon += (slope - 1);
			} // Next pixel becomes (x+1, y)
			else{
				epsilon += slope;
			}
		}
		if(std::abs(by - y - 0.5) + std::abs(bx - x - 0.5) <= 0.5 || bx > x + 0.5f){
			if(reversed){
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, va.fb_position.z);
			}else{
				point.fb_position = Vec3(x + 0.5f, y + 0.5f, vb.fb_position.z);
			}
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << x+0.5f << ", " << y+0.5f << std::endl;
		}
		return;
	}
	
	// General case slope > 1
	if(slope > 1){
		// Interchange the role of x and y
		temp = ax;
		ax = ay;
		ay = temp;

		temp = bx;
		bx = by;
		by = temp;

		int32_t x = std::floor(ax);
		int32_t y = std::floor(ay);

		// Epsilon is line's intersection at x = (pixel x) + 0.5, measure from (pixel y)
		float invslope = 1.0f/slope;
		float epsilon = ay - y + (x + 0.5 - ax) * invslope;
		float interpolate_z;
		
		std::cout << "Slope bigger than 1 case : " << slope << std::endl;

		
		if(y + invslope * (ax - x - 0.5) > ay) { x++; epsilon += invslope; }
		else if(ay + (x + 0.5 - ax) * invslope >= y + 1) { y+=signslope; epsilon -= 1; }
		else if(ax - x + ay - y > 1.5) { x++; y+=signslope; epsilon += (invslope - 1); }

		Fragment point;
		for( ; x <= std::floor(bx) - 1 ; x++){
			if(reversed){
				interpolate_z = (va.fb_position.z * (x + 0.5f - ax) + vb.fb_position.z * (bx - x - 0.5f))/(bx - ax);
			}else{
				interpolate_z = (vb.fb_position.z * (x + 0.5f - ax) + va.fb_position.z * (bx - x - 0.5f))/(bx - ax);
			}
			
			point.fb_position = Vec3(y + 0.5f, x + 0.5f, interpolate_z);
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << y+0.5f << ", " << x+0.5f << std::endl;

			// Next pixel becomes (x+1, y+1)
			if(epsilon > 1 - invslope) {
				y+=signslope;
				epsilon += (invslope - 1);
			} // Next pixel becomes (x+1, y)
			else{
				epsilon += invslope;
			}
		}
		if(std::abs(by - y - 0.5) + std::abs(bx - x - 0.5) <= 0.5 || bx > x + 0.5f){
			if(reversed){
				point.fb_position = Vec3(y + 0.5f, x + 0.5f, va.fb_position.z);
			}else{
				point.fb_position = Vec3(y + 0.5f, x + 0.5f, vb.fb_position.z);
			}
			point.attributes = va.attributes;
			point.derivatives.fill(Vec2(0.0f, 0.0f));
			emit_fragment(point);
			std::cout << "Point coordinate : " << y+0.5f << ", " << x+0.5f << std::endl;
		}
		return;
	}

}

/*
 * rasterize_triangle(a,b,c,emit) calls 'emit(frag)' at every location
 *  	(x+0.5,y+0.5) (where x,y are integers) covered by triangle (a,b,c).
 *
 * The emitted fragment should have:
 * - frag.fb_position.xy = (x+0.5, y+0.5)
 * - frag.fb_position.z = linearly interpolated fb_position.z from a,b,c (NOTE: does not depend on Interp mode!)
 * - frag.attributes = depends on Interp_* flag in flags:
 *   - if Interp_Flat: copy from va.attributes
 *   - if Interp_Smooth: interpolate as if (a,b,c) is a 2D triangle flat on the screen
 *   - if Interp_Correct: use perspective-correct interpolation
 * - frag.derivatives = derivatives w.r.t. fb_position.x and fb_position.y of the first frag.derivatives.size() attributes.
 *
 * Notes on derivatives:
 * 	The derivatives are partial derivatives w.r.t. screen locations. That is:
 *    derivatives[i].x = d/d(fb_position.x) attributes[i]
 *    derivatives[i].y = d/d(fb_position.y) attributes[i]
 *  You may compute these derivatives analytically or numerically.
 *
 *  See section 8.12.1 "Derivative Functions" of the GLSL 4.20 specification for some inspiration. (*HOWEVER*, the spec is solving a harder problem, and also nothing in the spec is binding on your implementation)
 *
 *  One approach is to rasterize blocks of four fragments and use forward and backward differences to compute derivatives.
 *  To assist you in this approach, keep in mind that the framebuffer size is *guaranteed* to be even. (see framebuffer.h)
 *
 * Notes on coverage:
 *  If two triangles are on opposite sides of the same edge, and a
 *  fragment center lies on that edge, rasterize_triangle should
 *  make sure that exactly one of the triangles emits that fragment.
 *  (Otherwise, speckles or cracks can appear in the final render.)
 * 
 *  For degenerate (co-linear) triangles, you may consider them to not be on any side of an edge.
 * 	Thus, even if two degnerate triangles share an edge that contains a fragment center, you don't need to emit it.
 *  You will not lose points for doing something reasonable when handling this case
 *
 *  This is pretty tricky to get exactly right!
 *
 */
template<PrimitiveType p, class P, uint32_t flags>
void Pipeline<p, P, flags>::rasterize_triangle(
	ClippedVertex const& va, ClippedVertex const& vb, ClippedVertex const& vc,
	std::function<void(Fragment const&)> const& emit_fragment) {
	// NOTE: it is okay to restructure this function to allow these tasks to use the
	//  same code paths. Be aware, however, that all of them need to remain working!
	//  (e.g., if you break Flat while implementing Correct, you won't get points
	//   for Flat.)
	if constexpr ((flags & PipelineMask_Interp) == Pipeline_Interp_Flat) {
		// A1T3: flat triangles
		// TODO: rasterize triangle (see block comment above this function).

		
		// Making va, vb, vc into counterclockwise order, with va be the most left side vertex. vb.y >= vc.y
		
			float ax, ay, az, bx, by, bz, cx, cy, cz, temp;
			if((vb.fb_position.x < vc.fb_position.x) && (vb.fb_position.x < va.fb_position.x)){
				ax = vb.fb_position.x; ay = vb.fb_position.y; az = vb.fb_position.z;
				if(va.fb_position.y >= vc.fb_position.y){
					bx = va.fb_position.x; by = va.fb_position.y; bz = va.fb_position.z;
					cx = vc.fb_position.x; cy = vc.fb_position.y; cz = vc.fb_position.z;
				}else{
					cx = va.fb_position.x; cy = va.fb_position.y; cz = va.fb_position.z;
					bx = vc.fb_position.x; by = vc.fb_position.y; bz = vc.fb_position.z;
				}
			}else if((vc.fb_position.x < vb.fb_position.x) && (vc.fb_position.x < va.fb_position.x)){
				ax = vc.fb_position.x; ay = vc.fb_position.y; az = vc.fb_position.z;
				if(va.fb_position.y >= vb.fb_position.y){
					bx = va.fb_position.x; by = va.fb_position.y; bz = va.fb_position.z;
					cx = vb.fb_position.x; cy = vb.fb_position.y; cz = vb.fb_position.z;
				}else{
					cx = va.fb_position.x; cy = va.fb_position.y; cz = va.fb_position.z;
					bx = vb.fb_position.x; by = vb.fb_position.y; bz = vb.fb_position.z;
				}
			}else{
				ax = va.fb_position.x; ay = va.fb_position.y; az = va.fb_position.z;
				if(vc.fb_position.y >= vb.fb_position.y){
					bx = vc.fb_position.x; by = vc.fb_position.y; bz = vc.fb_position.z;
					cx = vb.fb_position.x; cy = vb.fb_position.y; cz = vb.fb_position.z;
				}else{
					cx = vc.fb_position.x; cy = vc.fb_position.y; cz = vc.fb_position.z;
					bx = vb.fb_position.x; by = vb.fb_position.y; bz = vb.fb_position.z;
				}
			}
		

		// Corner case if two x values are the same.
		{
			if(std::abs(ax - bx) < FLT_EPSILON){
				float slope_high, slope_low, y_max, y_min, interpolate_z, lambda_1, lambda_2;
				Fragment point;
				if(ay > by){
					slope_high = (ay - cy) / (ax - cx); slope_low = (by - cy) / (bx - cx);
					y_max = ay + slope_high * (std::floor(ax) + 0.5 - ax); y_min = by + slope_low * (std::floor(bx) + 0.5 - bx);
				}else{
					slope_low = (ay - cy) / (ax - cx); slope_high = (by - cy) / (bx - cx);
					y_min = ay + slope_high * (std::floor(ax) + 0.5 - ax); y_max = by + slope_low * (std::floor(bx) + 0.5 - bx);
				}

				if(slope_high >= 0){
					// y_min < y + 0.5 <= y_max
					for(int x = std::floor(ax) ; x <= cx ; x++){
						for(int y = std::floor(y_min - 0.5f) + 1 ; y <= std::floor(y_max - 0.5f); y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}else if(slope_low < 0){
					// y_min <= y + 0.5 < y_max
					for(int x = std::floor(ax) ; x <= cx ; x++){
						for(int y = std::floor(0.5f - y_min) * (-1) ; y <= std::floor(0.5f - y_max) * (-1) - 1; y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}else{
					// y_min < y + 0.5 < y_max
					for(int x = std::floor(ax) ; x <= cx ; x++){
						for(int y = std::floor(y_min - 0.5f) + 1 ; y <= std::floor(0.5f - y_max) * (-1) - 1; y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}
				return;
			}else if(std::abs(ax - cx) < FLT_EPSILON){
				float slope_high, slope_low, y_max, y_min, interpolate_z, lambda_1, lambda_2;
				Fragment point;
				if(ay > cy){
					slope_high = (ay - by) / (ax - bx); slope_low = (cy - by) / (cx - bx);
					y_max = ay + slope_high * (std::floor(ax) + 0.5 - ax); y_min = cy + slope_low * (std::floor(cx) + 0.5 - cx);
				}else{
					slope_low = (ay - by) / (ax - bx); slope_high = (by - cy) / (bx - cx);
					y_min = ay + slope_high * (std::floor(ax) + 0.5 - ax); y_max = cy + slope_low * (std::floor(cx) + 0.5 - cx);
				}

				if(slope_high >= 0){
					// y_min < y + 0.5 <= y_max
					for(int x = std::floor(ax) ; x <= bx ; x++){
						for(int y = std::floor(y_min - 0.5f) + 1 ; y <= std::floor(y_max - 0.5f); y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}else if(slope_low < 0){
					// y_min <= y + 0.5 < y_max
					for(int x = std::floor(ax) ; x <= bx ; x++){
						for(int y = std::floor(0.5f - y_min) * (-1) ; y <= std::floor(0.5f - y_max) * (-1) - 1; y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}else{
					// y_min < y + 0.5 < y_max
					for(int x = std::floor(ax) ; x <= bx ; x++){
						for(int y = std::floor(y_min - 0.5f) + 1 ; y <= std::floor(0.5f - y_max) * (-1) - 1; y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}
				return;
			}else if(std::abs(bx - cx) < FLT_EPSILON){
				float slope_high, slope_low, y_max, y_min, interpolate_z, lambda_1, lambda_2;
				Fragment point;
				slope_high = (by - ay) / (bx - ax);
				slope_low = (cy - ay) / (cx - ax);
				y_max = ay + (std::floor(ax) + 0.5 - ax) * slope_high;
				y_min = ay + (std::floor(ax) + 0.5 - ax) * slope_low;
				if(slope_low >= 0){
					// y_min < y + 0.5 <= y_max
					for(int x = std::floor(ax) ; x <= bx ; x++){
						for(int y = std::floor(y_min - 0.5f) + 1 ; y <= std::floor(y_max - 0.5f); y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}else if(slope_high < 0){
					// y_min <= y + 0.5 < y_max
					for(int x = std::floor(ax) ; x <= bx ; x++){
						for(int y = std::floor(0.5f - y_min) * (-1) ; y <= std::floor(0.5f - y_max) * (-1) - 1; y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}else{
					// y_min <= y + 0.5 <= y_max
					for(int x = std::floor(ax) ; (x <= bx) ; x++){
						for(int y = std::floor(0.5f - y_min) * (-1) ; y <= std::floor(y_max - 0.5f); y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_low;
					}
				}
				return;
			}
		}

		// From va.x, increments x coordinate and rasterize pixels.
		{
			Fragment point;
			int x, y;
			float slope_high = (by - ay) / (bx - ax);
			float slope_low = (cy - ay) / (cx - ax);
			if(slope_high < slope_low){
				temp = slope_high;
				slope_high = slope_low;
				slope_low = temp;
			}
			float y_max = ay + slope_high * (std::floor(ax) + 0.5 - ax);
			float y_min = ay + slope_low * (std::floor(ax) + 0.5 - ax);
			float interpolate_z, lambda_1, lambda_2;

			
			if(slope_low >= 0){
				// y_min < y + 0.5 <= y_max
				for(x = std::floor(ax) ; (x <= bx) && (x <= cx) ; x++){
					for(y = std::floor(y_min - 0.5f) + 1 ; y <= std::floor(y_max - 0.5f); y++){
						lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
						lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
						interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
						point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
						point.attributes = va.attributes;
						point.derivatives.fill(Vec2(0.0f, 0.0f));
						emit_fragment(point);
						std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
					}
					y_max += slope_high;
					y_min += slope_low;
				}
			}else if(slope_high <= 0){
				// y_min <= y + 0.5 < y_max
				for(x = std::floor(ax) ; (x <= bx) && (x <= cx) ; x++){
					for(y = std::floor(0.5f - y_min) * (-1) ; y <= std::floor(0.5f - y_max) * (-1) - 1; y++){
						lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
						lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
						interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
						point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
						point.attributes = va.attributes;
						point.derivatives.fill(Vec2(0.0f, 0.0f));
						emit_fragment(point);
						std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
					}
					y_max += slope_high;
					y_min += slope_low;
				}
			}else{
				//y_min <= y + 0.5 <= y_max
				for(x = std::floor(ax) ; (x <= bx) && (x <= cx) ; x++){
					for(y = std::floor(0.5f - y_min) * (-1) ; y <= std::floor(y_max - 0.5f); y++){
						lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
						lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
						interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
						point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
						point.attributes = va.attributes;
						point.derivatives.fill(Vec2(0.0f, 0.0f));
						emit_fragment(point);
						std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
					}
					y_max += slope_high;
					y_min += slope_low;
				}
			}
			
			{
				float x_max;
				if(bx > cx){
					x_max = bx;
				}else{
					x_max = cx;
				}

				y_max -= slope_high;
				y_min -= slope_low;

				float slope_other = (by - cy) / (bx - cx);
				if(slope_other > 0){
					y_max += slope_high;
					y_min += slope_other;
					for( ; x <= x_max ; x++){
						// y_min < y + 0.5 <= y_max
						for(y = std::floor(y_min - 0.5) + 1 ; y <= std::floor(y_max - 0.5) ; y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_high;
						y_min += slope_other;
					}
				}else if(slope_other < 0){
					y_max += slope_other;
					y_min += slope_low;
					for( ; x <= x_max ; x++){
						// y_min < y + 0.5 <= y_max
						for(y = std::floor(y_min - 0.5) + 1 ; y <= std::floor(y_max - 0.5) ; y++){
							lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
							interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
							point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
							point.attributes = va.attributes;
							point.derivatives.fill(Vec2(0.0f, 0.0f));
							emit_fragment(point);
							std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
						}
						y_max += slope_other;
						y_min += slope_low;
					}
				}else{
					bool topedge = false;
					if(slope_high < 0){
						topedge = true;
					}
					if(topedge){
						y_max += slope_high;
						for( ; x <= x_max ; x++){
							// y_min <= y + 0.5 <= y_max 
							for(y = std::floor(0.5 - y_min) * (-1) ; y <= std::floor(y_max - 0.5) ; y++){
								lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
								lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
								interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
								point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
								point.attributes = va.attributes;
								point.derivatives.fill(Vec2(0.0f, 0.0f));
								emit_fragment(point);
								std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
							}
							y_max += slope_high;
						}
					}else{
						y_min += slope_low;
						for( ; x <= x_max ; x++){
							// y_min < y + 0.5 <= y_max 
							for(y = std::floor(y_min - 0.5) + 1 ; y <= std::floor(y_max - 0.5) ; y++){
								lambda_1 = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
								lambda_2 = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / ((by - cy) * (ax - cx) + (cx - bx) * (ay - cy));
								interpolate_z = lambda_1 * az + lambda_2 * bz + (1 - lambda_1 - lambda_2) * cz;
								point.fb_position = Vec3(x + 0.5f, y + 0.5f, interpolate_z);
								point.attributes = va.attributes;
								point.derivatives.fill(Vec2(0.0f, 0.0f));
								emit_fragment(point);
								std::cout << "Point : " << x + 0.5 << " " << y + 0.5 << std::endl;
							}
							y_min += slope_low;
						}
					}		
				}
			}


		}

	} else if constexpr ((flags & PipelineMask_Interp) == Pipeline_Interp_Smooth) {
		// A1T5: screen-space smooth triangles
		// TODO: rasterize triangle (see block comment above this function).

		// As a placeholder, here's code that calls the Flat interpolation version of the function:
		//(remove this and replace it with a real solution)
		Pipeline<PrimitiveType::Lines, P, (flags & ~PipelineMask_Interp) | Pipeline_Interp_Flat>::rasterize_triangle(va, vb, vc, emit_fragment);
	} else if constexpr ((flags & PipelineMask_Interp) == Pipeline_Interp_Correct) {
		// A1T5: perspective correct triangles
		// TODO: rasterize triangle (block comment above this function).

		// As a placeholder, here's code that calls the Screen-space interpolation function:
		//(remove this and replace it with a real solution)
		Pipeline<PrimitiveType::Lines, P, (flags & ~PipelineMask_Interp) | Pipeline_Interp_Smooth>::rasterize_triangle(va, vb, vc, emit_fragment);
	}
}

//-------------------------------------------------------------------------
// compile instantiations for all programs and blending and testing types:

#include "programs.h"

template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Always | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Always | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Always | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Never | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Never | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Never | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Less | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Less | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Less | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Always | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Always | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Always | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Never | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Never | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Never | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Less | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Less | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Less | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Always | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Always | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Always | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Never | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Never | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Never | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Less | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Less | Pipeline_Interp_Smooth>;
template struct Pipeline<PrimitiveType::Triangles, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Less | Pipeline_Interp_Correct>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Always | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Never | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Replace | Pipeline_Depth_Less | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Always | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Never | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Add | Pipeline_Depth_Less | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Always | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Never | Pipeline_Interp_Flat>;
template struct Pipeline<PrimitiveType::Lines, Programs::Lambertian,
                         Pipeline_Blend_Over | Pipeline_Depth_Less | Pipeline_Interp_Flat>;