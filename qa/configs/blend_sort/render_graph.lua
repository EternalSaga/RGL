-- 本靶的渲染图：**空表 = 内建 deferred**（含 transparent 段）。
--
-- 🔴 这里**不能**写自定义 attachments/passes：内建 deferred 是唯一带
-- draws = "transparent" 那一段的图，换成自定义图而漏了它，装配期 validateBlendSupport
-- 会直接抛（这条对账是刀2 立的）。写 return {} 而不是删掉本文件 = 让"走内建图"这件事
-- 在目录里可见，而不是靠缺文件回落。
return {}
