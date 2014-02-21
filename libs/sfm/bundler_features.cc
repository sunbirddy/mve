#include "mve/image_tools.h"
#include "sfm/bundler_common.h"
#include "sfm/bundler_features.h"

SFM_NAMESPACE_BEGIN
SFM_BUNDLER_NAMESPACE_BEGIN

void
Features::compute (mve::Scene::Ptr scene, FeatureType type,
    ViewportList* viewports)
{
    if (scene == NULL)
        throw std::invalid_argument("NULL scene given");

    if (viewports == NULL && this->opts.feature_embedding.empty())
        throw std::invalid_argument("No viewports or feature embedding given");

    mve::Scene::ViewList const& views = scene->get_views();

    /* Initialize viewports. */
    if (viewports != NULL)
    {
        viewports->clear();
        viewports->resize(views.size());
    }

    /* Iterate the scene and compute features. */
#pragma omp parallel for schedule(dynamic,1)
    for (std::size_t i = 0; i < views.size(); ++i)
    {
        if (views[i] == NULL)
            continue;

        Viewport* viewport = (viewports == NULL ? NULL : &viewports->at(i));
        mve::View::Ptr view = views[i];

        switch (type)
        {
            case SIFT_FEATURES:
                this->compute<Sift>(view, viewport);
                break;

            case SURF_FEATURES:
                this->compute<Surf>(view, viewport);
                break;

            default:
                throw std::invalid_argument("Invalid feature type");
        }
    }
}

template <>
sfm::Sift
Features::construct<sfm::Sift> (void) const
{
    return sfm::Sift(this->opts.sift_options);
}

template <>
sfm::Surf
Features::construct<sfm::Surf> (void) const
{
    return sfm::Surf(this->opts.surf_options);
}

template <>
int
Features::descriptor_length<sfm::Sift> (void) const
{
    return 128;
}

template <>
int
Features::descriptor_length<sfm::Surf> (void) const
{
    return 64;
}

template <typename FEATURE>
void
Features::compute (mve::View::Ptr view, Viewport* viewport) const
{
    /* Check if descriptors can be loaded from embedding. */
    typename FEATURE::Descriptors descriptors;
    bool has_data = view->has_data_embedding(this->opts.feature_embedding);
    if (!this->opts.force_recompute && has_data)
    {
        /* If features exist but viewport is not given, skip computation. */
        if (viewport == NULL)
            return;

        /* Otherwise load descriptors from embedding. */
        mve::ByteImage::Ptr data = view->get_data(this->opts.feature_embedding);
        sfm::bundler::embedding_to_descriptors(data, &descriptors,
            &viewport->width, &viewport->height);
    }

    /*
     * Load color image either for computation of features or coloring
     * the loaded descriptors. In the latter case the image needs to be
     * rescaled to the image size descriptors have been computed from.
     */
    mve::ByteImage::Ptr img = view->get_byte_image(this->opts.image_embedding);
    if (descriptors.empty())
    {
        std::cout << "Computing features for view ID " << view->get_id()
            << " (" << img->width() << "x" << img->height()
            << ")..." << std::endl;

        /* Rescale image until maximum image size is met. */
        bool was_scaled = false;
        while (img->width() * img->height() > this->opts.max_image_size)
        {
            was_scaled = true;
            img = mve::image::rescale_half_size<uint8_t>(img);
        }
        if (was_scaled)
        {
            std::cout << "  scaled to " << img->width() << "x" << img->height()
                << " pixels." << std::endl;
        }

        FEATURE feature = this->construct<FEATURE>();
        feature.set_image(img);
        feature.process();
        descriptors = feature.get_descriptors();
    }
    else
    {
        /* Rescale image to exactly the descriptor image size. */
        while (img->width() > viewport->width
            && img->height() > viewport->height)
            img = mve::image::rescale_half_size<uint8_t>(img);
        if (img->width() != viewport->width
            || img->height() != viewport->height)
            throw std::runtime_error("Error rescaling image to match descriptors");
    }

    /* Update feature embedding if requested. */
    if (!this->opts.feature_embedding.empty())
    {
        mve::ByteImage::Ptr descr_data = descriptors_to_embedding
            (descriptors, img->width(), img->height());
        view->set_data(this->opts.feature_embedding, descr_data);
        if (!this->opts.skip_saving_views)
            view->save_mve_file();
    }

    /* Initialize viewports. */
    if (viewport != NULL)
    {
        int const descr_len = this->descriptor_length<FEATURE>();
        viewport->descr_data.allocate(descriptors.size() * descr_len);
        viewport->positions.resize(descriptors.size());
        viewport->colors.resize(descriptors.size());

        float* ptr = viewport->descr_data.begin();
        for (std::size_t i = 0; i < descriptors.size(); ++i, ptr += descr_len)
        {
            typename FEATURE::Descriptor const& d = descriptors[i];
            std::copy(d.data.begin(), d.data.end(), ptr);
            viewport->positions[i] = math::Vec2f(d.x, d.y);
            img->linear_at(d.x, d.y, viewport->colors[i].begin());
        }
    }

    view->cache_cleanup();
}

SFM_BUNDLER_NAMESPACE_END
SFM_NAMESPACE_END
