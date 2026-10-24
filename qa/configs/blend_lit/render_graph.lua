-- 本靶的渲染图：**空表 = 内建 deferred**（含 transparent 段）。
--
-- 🔴 这里**不能**写自定义 attachments/passes：内建 deferred 是唯一带
-- draws = "transparent" 那一段的图，换成自定义图而漏了它，装配期 validateBlendSupport
-- 会直接抛（这条对账是刀2 立的）。写 return {} 而不是删掉本文件 = 让"走内建图"这件事
-- 在目录里可见，而不是靠缺文件回落。
--
-- 本靶的透明段条数读数（render.transparentDraws）就是这一段 draw list 的 span 长度，
-- 所以「图里有没有 transparent 段」与「判据能不能读到条数」是同一件事的两面。
return {}
