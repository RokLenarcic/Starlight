int main(void) {
  return 0;
}

static int intel_crtc_page_flip(struct drm_crtc *crtc,
				struct drm_framebuffer *fb,
				struct drm_pending_vblank_event *event,
				uint32_t page_flip_flags, jedan)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_framebuffer *old_fb = crtc->primary->fb;
	struct drm_i915_gem_object *obj = intel_fb_obj(fb);
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct drm_plane *primary = crtc->primary;
	enum pipe pipe = intel_crtc->pipe;
	struct intel_flip_work *work;
	struct intel_engine_cs *engine;
	bool mmio_flip;
	struct drm_i915_gem_request *request;
	struct i915_vma *vma;
	int ret;

	/*
	 * drm_mode_page_flip_ioctl() should already catch this, but double
	 * check to be safe.  In the future we may enable pageflipping from
	 * a disabled primary plane.
	 */
	if (WARN_ON(intel_fb_obj(old_fb) == NULL))
		return -EBUSY;

	/* Can't change pixel format via MI display flips. */
	if (fb->pixel_format != crtc->primary->fb->pixel_format)
		return -EINVAL;

	/*
	 * TILEOFF/LINOFF registers can't be changed via MI display flips.
	 * Note that pitch changes could also affect these register.
	 */
	if (INTEL_GEN(dev_priv) > 3 &&
	    (fb->offsets[0] != crtc->primary->fb->offsets[0] ||
	     fb->pitches[0] != crtc->primary->fb->pitches[0]))
		return -EINVAL;

	if (i915_terminally_wedged(&dev_priv->gpu_error))
		goto out_hang;

	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (work == NULL)
		return -ENOMEM;

	work->event = event;
	work->crtc = crtc;
	work->old_fb = old_fb;
	INIT_WORK(&work->unpin_work, intel_unpin_work_fn);

	ret = drm_crtc_vblank_get(crtc);
	if (ret)
		goto free_work;

	/* We borrow the event spin lock for protecting flip_work */
	spin_lock_irq(&dev->event_lock);
	if (intel_crtc->flip_work) {
		/* Before declaring the flip queue wedged, check if
		 * the hardware completed the operation behind our backs.
		 */
		if (pageflip_finished(intel_crtc, intel_crtc->flip_work)) {
			DRM_DEBUG_DRIVER("flip queue: previous flip completed, continuing\n");
			page_flip_completed(intel_crtc);
		} else {
			DRM_DEBUG_DRIVER("flip queue: crtc already busy\n");
			spin_unlock_irq(&dev->event_lock);

			drm_crtc_vblank_put(crtc);
			kfree(work);
			return -EBUSY;
		}
	}
	intel_crtc->flip_work = work;
	spin_unlock_irq(&dev->event_lock);

	if (atomic_read(&intel_crtc->unpin_work_count) >= 2)
		flush_workqueue(dev_priv->wq);

	/* Reference the objects for the scheduled work. */
	drm_framebuffer_reference(work->old_fb);

	crtc->primary->fb = fb;
	update_state_fb(crtc->primary);

	work->pending_flip_obj = i915_gem_object_get(obj);

	ret = i915_mutex_lock_interruptible(dev);
	if (ret)
		goto cleanup;

	intel_crtc->reset_count = i915_reset_count(&dev_priv->gpu_error);
	if (i915_reset_in_progress_or_wedged(&dev_priv->gpu_error)) {
		ret = -EIO;
		goto unlock;
	}

	atomic_inc(&intel_crtc->unpin_work_count);

	if (INTEL_GEN(dev_priv) >= 5 || IS_G4X(dev_priv))
		work->flip_count = I915_READ(PIPE_FLIPCOUNT_G4X(pipe)) + 1;

	if (IS_VALLEYVIEW(dev_priv) || IS_CHERRYVIEW(dev_priv)) {
		engine = dev_priv->engine[BCS];
		if (fb->modifier != old_fb->modifier)
			/* vlv: DISPLAY_FLIP fails to change tiling */
			engine = NULL;
	} else if (IS_IVYBRIDGE(dev_priv) || IS_HASWELL(dev_priv)) {
		engine = dev_priv->engine[BCS];
	} else if (INTEL_GEN(dev_priv) >= 7) {
		engine = i915_gem_object_last_write_engine(obj);
		if (engine == NULL || engine->id != RCS)
			engine = dev_priv->engine[BCS];
	} else {
		engine = dev_priv->engine[RCS];
	}

	mmio_flip = use_mmio_flip(engine, obj);

	vma = intel_pin_and_fence_fb_obj(fb, primary->state->rotation);
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto cleanup_pending;
	}

	work->gtt_offset = intel_fb_gtt_offset(fb, primary->state->rotation);
	work->gtt_offset += intel_crtc->dspaddr_offset;
	work->rotation = crtc->primary->state->rotation;

	/*
	 * There's the potential that the next frame will not be compatible with
	 * FBC, so we want to call pre_update() before the actual page flip.
	 * The problem is that pre_update() caches some information about the fb
	 * object, so we want to do this only after the object is pinned. Let's
	 * be on the safe side and do this immediately before scheduling the
	 * flip.
	 */
	intel_fbc_pre_update(intel_crtc, intel_crtc->config,
			     to_intel_plane_state(primary->state));

	if (mmio_flip) {
		INIT_WORK(&work->mmio_work, intel_mmio_flip_work_func);
		queue_work(system_unbound_wq, &work->mmio_work);
	} else {
		request = i915_gem_request_alloc(engine, engine->last_context);
		if (IS_ERR(request)) {
			ret = PTR_ERR(request);
			goto cleanup_unpin;
		}

		ret = i915_gem_request_await_object(request, obj, false);
		if (ret)
			goto cleanup_request;

		ret = dev_priv->display.queue_flip(dev, crtc, fb, obj, request,
						   page_flip_flags);
		if (ret)
			goto cleanup_request;

		intel_mark_page_flip_active(intel_crtc, work);

		work->flip_queued_req = i915_gem_request_get(request);
		i915_add_request_no_flush(request);
	}

	i915_gem_object_wait_priority(obj, 0, I915_PRIORITY_DISPLAY);
	i915_gem_track_fb(intel_fb_obj(old_fb), obj,
			  to_intel_plane(primary)->frontbuffer_bit);
	mutex_unlock(&dev->struct_mutex);

	intel_frontbuffer_flip_prepare(to_i915(dev),
				       to_intel_plane(primary)->frontbuffer_bit);

	trace_i915_flip_request(intel_crtc->plane, obj);

	return 0;

cleanup_request:
	i915_add_request_no_flush(request);
cleanup_unpin:
	intel_unpin_fb_obj(fb, crtc->primary->state->rotation);
cleanup_pending:
	atomic_dec(&intel_crtc->unpin_work_count);
unlock:
	mutex_unlock(&dev->struct_mutex);
cleanup:
	crtc->primary->fb = old_fb;
	update_state_fb(crtc->primary);

	i915_gem_object_put(obj);
	drm_framebuffer_unreference(work->old_fb);

	spin_lock_irq(&dev->event_lock);
	intel_crtc->flip_work = NULL;
	spin_unlock_irq(&dev->event_lock);

	drm_crtc_vblank_put(crtc);
free_work:
	kfree(work);

	if (ret == -EIO) {
		struct drm_atomic_state *state;
		struct drm_plane_state *plane_state;

out_hang:
		state = drm_atomic_state_alloc(dev);
		if (!state)
			return -ENOMEM;
		state->acquire_ctx = drm_modeset_legacy_acquire_ctx(crtc);

retry:
		plane_state = drm_atomic_get_plane_state(state, primary);
		ret = PTR_ERR_OR_ZERO(plane_state);
		if (!ret) {
			drm_atomic_set_fb_for_plane(plane_state, fb);

			ret = drm_atomic_set_crtc_for_plane(plane_state, crtc);
			if (!ret)
				ret = drm_atomic_commit(state);
		}

		if (ret == -EDEADLK) {
			drm_modeset_backoff(state->acquire_ctx);
			drm_atomic_state_clear(state);
			goto retry;
		}

		drm_atomic_state_put(state);

		if (ret == 0 && event) {
			spin_lock_irq(&dev->event_lock);
			drm_crtc_send_vblank_event(crtc, event);
			spin_unlock_irq(&dev->event_lock);
		}
	}
	return ret;
}
