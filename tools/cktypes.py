"""Parameter type GUIDs (Virtools SDK CKPGUID_*) with their parents (CK2.dll type registration, class
hierarchy for object types). Shared by the op resolver and docs."""
G = lambda s: tuple(int(x, 16) for x in s.split(':'))
TYPES = {  # guid: (name, parent)
    '1cb10760:419f50c5': ('None', None),
    '47884c3f:432c2c20': ('Float', None), 'f3c84b4e:0ffacc34': ('Percentage', 'Float'),
    '11262cf5:30b0233a': ('Angle', 'Float'), '54b4422b:730f0f4f': ('Time', 'Float'),
    '5a5716fd:44e276d7': ('Integer', None), '1ad52a8e:5e741920': ('Boolean', 'Integer'),
    '6bd010e2:115617ea': ('String', None), '48824eae:2fe47960': ('Vector', None),
    '13b01b3c:1942583e': ('Euler', 'Vector'), '06c439ee:45b50fc2': ('Quaternion', None),
    '7ab20d20:693044a9': ('Rect', None), '4efcb34a:6079e42f': ('Vector2D', None),
    '643f046e:65211b71': ('Matrix', None), '57d42fee:7cbb3b91': ('Color', None),
    '668649c8:283e2ee1': ('Box', None), '03881e12:5ba34e2b': ('Message', None),
    '3ea34ee9:09fa5366': ('Attribute', None), '20ad345d:1afb25b1': ('2DCurve', None),
    # object types (class association) and their parents
    '30ec20ab:6df6517d': ('Object', None), '71d80779:402f42f3': ('BeObject', 'Object'),
    '7ea4176d:1b405d30': ('Script', 'Object'), '5b8a05d5:31ea28d4': ('3DEntity', 'BeObject'),
    '362e4df8:17443539': ('3DObject', '3DEntity'), '3cf24d6f:216204f9': ('Camera', '3DEntity'),
    '4b6d412f:5d1e1416': ('Light', '3DEntity'), '181671e3:1bdc1503': ('2DEntity', 'BeObject'),
    '55ab12cd:22ae6a8b': ('Material', 'BeObject'), '155b2870:183679f8': ('Texture', 'BeObject'),
    '24535345:65d15229': ('Mesh', 'BeObject'), '5c0f151b:6f0547fd': ('Group', 'BeObject'),
    '024d52f1:678223b2': ('DataArray', 'BeObject'), '40194410:3a773f80': ('Sound', 'BeObject'),
    '4bf74e5e:45f409ef': ('WaveSound', 'Sound'), '10584787:76932f77': ('Level', 'BeObject'),
    '71df7142:c437133a': ('ObjectArray', None),
}
BYNAME = {v[0]: k for k, v in TYPES.items()}
def parent(g):
    t = TYPES.get(g)
    return BYNAME[t[1]] if t and t[1] else None
def name(g):
    return TYPES.get(g, (g,))[0]
