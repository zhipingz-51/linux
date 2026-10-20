// SPDX-License-Identifier: GPL-2.0
/* Tests for mapping-scoped TPH queries and their invalidation lifetime. */
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/dma-buf.h>
#include <linux/dma-buf-mapping.h>
#include <linux/dma-resv.h>
#include <linux/module.h>

struct tph_test_state {
	u16 tag[2];
	u8 ph;
	bool eligible;
	int error;
	unsigned int maps;
	unsigned int unmaps;
};

struct tph_test_mapping {
	struct sg_table sgt;
	struct dma_buf_attachment *attach;
	u16 tag[2];
	u8 ph;
	bool eligible;
};

struct tph_test_import {
	struct sg_table *live;
};

struct tph_test_fixture {
	struct tph_test_state state;
	struct dma_buf *dmabuf;
	struct device *dev;
	struct dma_buf_attachment *attach[2];
	struct tph_test_import imports[2];
};

static struct sg_table *tph_test_map(struct dma_buf_attachment *attach,
				   enum dma_data_direction dir)
{
	struct tph_test_state *state = attach->dmabuf->priv;
	struct tph_test_mapping *map;
	int ret;

	map = kzalloc_obj(*map);
	if (!map)
		return ERR_PTR(-ENOMEM);
	ret = sg_alloc_table(&map->sgt, 1, GFP_KERNEL);
	if (ret) {
		kfree(map);
		return ERR_PTR(ret);
	}
	sg_dma_address(map->sgt.sgl) = PAGE_SIZE;
	sg_dma_len(map->sgt.sgl) = PAGE_SIZE;
	map->attach = attach;
	memcpy(map->tag, state->tag, sizeof(map->tag));
	map->ph = state->ph;
	map->eligible = state->eligible;
	state->maps++;
	return &map->sgt;
}

static void tph_test_unmap(struct dma_buf_attachment *attach,
			   struct sg_table *sgt, enum dma_data_direction dir)
{
	struct tph_test_state *state = attach->dmabuf->priv;
	struct tph_test_mapping *map =
		container_of(sgt, struct tph_test_mapping, sgt);

	state->unmaps++;
	sg_free_table(sgt);
	kfree(map);
}

static int tph_test_query(struct dma_buf_attachment *attach,
			  struct sg_table *sgt, bool extended, u16 *tag, u8 *ph)
{
	struct tph_test_state *state = attach->dmabuf->priv;
	struct tph_test_mapping *map =
		container_of(sgt, struct tph_test_mapping, sgt);

	/* This must receive the original SGT, including with DMABUF_DEBUG=y. */
	if (map->attach != attach)
		return -EINVAL;
	if (state->error)
		return state->error;
	if (!map->eligible)
		return -EOPNOTSUPP;
	*tag = map->tag[extended];
	*ph = map->ph;
	return 0;
}

static const struct dma_buf_mapping_sgt_exp_ops tph_test_sgt_ops = {
	.map_dma_buf = tph_test_map,
	.unmap_dma_buf = tph_test_unmap,
};

static const struct dma_buf_mapping_match tph_test_export_match =
	DMA_BUF_EMAPPING_SGT(&tph_test_sgt_ops);

static void tph_test_release(struct dma_buf *dmabuf)
{
}

static const struct dma_buf_ops tph_test_export_ops = {
	.single_exporter_match = &tph_test_export_match,
	.release = tph_test_release,
	.get_pci_tph = tph_test_query,
};

static const struct dma_buf_ops tph_test_no_hint_ops = {
	.single_exporter_match = &tph_test_export_match,
	.release = tph_test_release,
};

static void tph_test_invalidate(struct dma_buf_attachment *attach)
{
	struct tph_test_import *imp = attach->importer_priv;

	if (!imp->live)
		return;
	dma_buf_sgt_unmap_attachment(attach, imp->live, DMA_BIDIRECTIONAL);
	imp->live = NULL;
}

static const struct dma_buf_attach_ops tph_test_import_ops = {
	.allow_peer2peer = true,
	.invalidate_mappings = tph_test_invalidate,
};

static void tph_test_cleanup(void *data)
{
	struct tph_test_fixture *f = data;
	int i;

	for (i = 0; i < ARRAY_SIZE(f->attach); i++) {
		if (IS_ERR_OR_NULL(f->attach[i]))
			continue;
		dma_resv_lock(f->dmabuf->resv, NULL);
		tph_test_invalidate(f->attach[i]);
		dma_resv_unlock(f->dmabuf->resv);
		dma_buf_detach(f->dmabuf, f->attach[i]);
	}
	dma_buf_put(f->dmabuf);
}

static int tph_test_init(struct kunit *test)
{
	struct tph_test_fixture *f;
	struct dma_buf_export_info exp = {
		.exp_name = "tph-mapping-test",
		.owner = THIS_MODULE,
		.size = PAGE_SIZE,
		.flags = O_RDWR,
	};
	int ret;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	test->priv = f;
	f->dev = kunit_device_register(test, "tph-importer");
	if (IS_ERR(f->dev))
		return PTR_ERR(f->dev);
	f->state.tag[0] = 0;
	f->state.tag[1] = 0xabcd;
	f->state.ph = 2;
	f->state.eligible = true;
	exp.ops = &tph_test_export_ops;
	exp.priv = &f->state;
	f->dmabuf = dma_buf_export(&exp);
	if (IS_ERR(f->dmabuf))
		return PTR_ERR(f->dmabuf);
	ret = kunit_add_action_or_reset(test, tph_test_cleanup, f);
	if (ret)
		return ret;
	f->attach[0] = dma_buf_sgt_dynamic_attach(f->dmabuf, f->dev,
					       &tph_test_import_ops, &f->imports[0]);
	return IS_ERR(f->attach[0]) ? PTR_ERR(f->attach[0]) : 0;
}

static void tph_test_remap(struct kunit *test, unsigned int index)
{
	struct tph_test_fixture *f = test->priv;
	struct tph_test_import *imp = &f->imports[index];

	tph_test_invalidate(f->attach[index]);
	imp->live = dma_buf_sgt_map_attachment(f->attach[index], DMA_BIDIRECTIONAL);
	if (IS_ERR(imp->live)) {
		KUNIT_FAIL(test, "map failed: %ld", PTR_ERR(imp->live));
		imp->live = NULL;
	}
}

static void tph_test_mapping_snapshot(struct kunit *test)
{
	struct tph_test_fixture *f = test->priv;
	u16 tag = 1;
	u8 ph = 0;

	dma_resv_lock(f->dmabuf->resv, NULL);
	tph_test_remap(test, 0);
	f->state.tag[0] = 0x12;
	f->state.ph = 3;
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[0], f->imports[0].live,
						false, &tag, &ph), 0);
	KUNIT_EXPECT_EQ(test, tag, 0);
	KUNIT_EXPECT_EQ(test, ph, 2);
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[0], f->imports[0].live,
						true, &tag, &ph), 0);
	KUNIT_EXPECT_EQ(test, tag, 0xabcd);
	dma_resv_unlock(f->dmabuf->resv);
}

static void tph_test_attachment_scope(struct kunit *test)
{
	struct tph_test_fixture *f = test->priv;
	u16 tag;
	u8 ph;

	f->attach[1] = dma_buf_sgt_dynamic_attach(f->dmabuf, f->dev,
					       &tph_test_import_ops, &f->imports[1]);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->attach[1]);
	dma_resv_lock(f->dmabuf->resv, NULL);
	tph_test_remap(test, 0);
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[1], f->imports[0].live,
						false, &tag, &ph), -EINVAL);
	f->state.tag[0] = 0x12;
	tph_test_remap(test, 1);
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[1], f->imports[1].live,
						false, &tag, &ph), 0);
	KUNIT_EXPECT_EQ(test, tag, 0x12);
	dma_resv_unlock(f->dmabuf->resv);
}

static void tph_test_invalidation_fallback(struct kunit *test)
{
	struct tph_test_fixture *f = test->priv;
	u16 tag;
	u8 ph;

	dma_resv_lock(f->dmabuf->resv, NULL);
	tph_test_remap(test, 0);
	dma_buf_invalidate_mappings(f->dmabuf);
	KUNIT_EXPECT_PTR_EQ(test, f->imports[0].live, NULL);
	KUNIT_EXPECT_EQ(test, f->state.unmaps, 1);
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[0], NULL,
						false, &tag, &ph), -EINVAL);
	f->state.eligible = false;
	tph_test_remap(test, 0);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, f->imports[0].live);
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[0], f->imports[0].live,
						false, &tag, &ph), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, f->state.maps, 2);
	KUNIT_EXPECT_EQ(test, f->state.unmaps, 1);
	dma_resv_unlock(f->dmabuf->resv);
}

static void tph_test_errors_and_values(struct kunit *test)
{
	struct tph_test_fixture *f = test->priv;
	u16 tag;
	u8 ph;

	dma_resv_lock(f->dmabuf->resv, NULL);
	tph_test_remap(test, 0);
	f->state.error = -EIO;
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[0], f->imports[0].live,
						false, &tag, &ph), -EIO);
	f->state.error = 0;
	f->state.tag[0] = 0x100;
	tph_test_remap(test, 0);
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[0], f->imports[0].live,
						false, &tag, &ph), -EINVAL);
	f->state.ph = 4;
	tph_test_remap(test, 0);
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[0], f->imports[0].live,
						true, &tag, &ph), -EINVAL);
	f->dmabuf->ops = &tph_test_no_hint_ops;
	KUNIT_EXPECT_EQ(test, dma_buf_get_pci_tph(f->attach[0], f->imports[0].live,
						false, &tag, &ph), -EOPNOTSUPP);
	dma_resv_unlock(f->dmabuf->resv);
}

static struct kunit_case tph_test_cases[] = {
	KUNIT_CASE(tph_test_mapping_snapshot),
	KUNIT_CASE(tph_test_attachment_scope),
	KUNIT_CASE(tph_test_invalidation_fallback),
	KUNIT_CASE(tph_test_errors_and_values),
	{}
};

static struct kunit_suite tph_test_suite = {
	.name = "dma_buf_tph",
	.init = tph_test_init,
	.test_cases = tph_test_cases,
};
kunit_test_suite(tph_test_suite);
MODULE_IMPORT_NS("DMA_BUF");
MODULE_LICENSE("GPL");
